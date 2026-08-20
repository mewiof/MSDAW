#include "PrecompHeader.h"
#include "WarpEngine.h"
#include <cmath>
#include <algorithm>

namespace {

	constexpr double kPi = 3.14159265358979323846;

	// biggest block we ever render in one pass; larger requests are tiled. this bounds the
	// stack scratch so the audio thread never allocates. audio blocks are 512 in practice
	constexpr int kMaxTile = 1024;

	// grains overlapping a tile are few (hop is always hundreds of output frames), but size
	// the array for the worst plausible case (short grains at a low sample rate)
	constexpr int kMaxGrains = 128;

	// taps used to correlate a candidate grain against the previous one; coarse on purpose
	constexpr int kCorrTaps = 24;

	// candidate source offsets tried per grain in the similarity search (odd so 0 is centered)
	constexpr int kWsolaCandidates = 21;

	// per-mode geometry resolved from the params once per block
	struct ModeConfig {
		double grainOut = 0.0; // grain length in output frames
		double hop = 0.0;	   // synthesis hop in output frames (grain / overlap)
		double halfLen = 0.0;  // half a grain, the window's reach from its center
		double rampLen = 0.0;  // window rise/fall length; == halfLen gives a full Hann
		bool useWsola = false;
		bool useFluct = false;
		bool useFormants = false;
	};

	// linear-interpolated read of one source channel at a fractional frame; out of range is
	// silence, so grains that run past the file end simply fade out
	inline float ReadSampleLinear(const float* s, size_t n, int clipChannels, int channel, double pos) {
		if (pos < 0.0)
			return 0.0f;
		int64_t i = (int64_t)pos;
		double frac = pos - (double)i;
		size_t idx1 = (size_t)(i * clipChannels + channel);
		size_t idx2 = idx1 + (size_t)clipChannels;
		if (idx2 >= n)
			return 0.0f;
		return s[idx1] + (float)frac * (s[idx2] - s[idx1]);
	}

	// raised-cosine window with a flat top: 0 at the edges, ramps over rampLen, then plateaus
	// rampLen == halfLen collapses the plateau and yields a Hann; a short rampLen keeps a
	// near-rectangular grain that preserves transient punch (used by Beats)
	inline double GrainWindow(double local, double halfLen, double rampLen) {
		double d = halfLen - std::abs(local); // distance in from the nearest edge
		if (d <= 0.0)
			return 0.0;
		if (d >= rampLen)
			return 1.0;
		return 0.5 * (1.0 - std::cos(kPi * d / rampLen));
	}

	// cheap integer hash so Texture's per-grain jitter is random-looking yet fully
	// reproducible from the grain index (determinism is what keeps the engine seekable)
	inline uint32_t Hash32(uint32_t x) {
		x ^= x >> 16;
		x *= 0x7feb352dU;
		x ^= x >> 15;
		x *= 0x846ca68bU;
		x ^= x >> 16;
		return x;
	}

	// jitter in [-1, 1] for grain j
	inline double GrainJitter(int64_t j) {
		uint32_t h = Hash32((uint32_t)(j * 2654435761U));
		return ((double)h / 4294967295.0) * 2.0 - 1.0;
	}

	ModeConfig ResolveMode(const WarpRenderParams& p) {
		ModeConfig c;
		double sr = (p.sampleRate > 0.0) ? p.sampleRate : 48000.0;
		double grainMs = p.grainSizeMs;
		int overlap = 4;

		switch (p.mode) {
		case WarpMode::Beats:
			// short grains + a mostly-flat window keep drum transients crisp; the user's
			// "transient envelope" only lengthens the fades toward a smoother Hann
			grainMs = 55.0;
			overlap = 2;
			break;
		case WarpMode::Tones:
			grainMs = std::clamp(grainMs, 20.0, 200.0);
			overlap = 4;
			c.useWsola = true;
			break;
		case WarpMode::Texture:
			grainMs = std::clamp(grainMs, 40.0, 300.0);
			overlap = 4;
			c.useFluct = true;
			break;
		case WarpMode::Complex:
			grainMs = std::clamp(grainMs, 40.0, 200.0);
			overlap = 4;
			c.useWsola = true;
			break;
		case WarpMode::ComplexPro:
			grainMs = std::clamp(grainMs, 40.0, 200.0);
			overlap = 4;
			c.useWsola = true;
			c.useFormants = true;
			break;
		default: // RePitch never reaches the engine, but keep a sane fallback
			grainMs = std::clamp(grainMs, 20.0, 200.0);
			overlap = 4;
			break;
		}

		c.grainOut = std::max(8.0, grainMs * 0.001 * sr);
		c.hop = c.grainOut / (double)overlap;
		c.halfLen = c.grainOut * 0.5;

		if (p.mode == WarpMode::Beats) {
			// map 0..1 onto "very short fade" .. "full Hann"; a floor avoids audible clicks
			double minRamp = std::min(c.halfLen, std::max(2.0, 0.001 * sr)); // ~1 ms
			c.rampLen = minRamp + (c.halfLen - minRamp) * std::clamp(p.transientEnvelope, 0.0, 1.0);
		} else {
			c.rampLen = c.halfLen; // full Hann
		}
		return c;
	}

	// the source frame a grain centered at output frame cj reads from, before any per-grain
	// offset. this is the pitch-independent time map that pins the clip to the grid
	inline double GrainAnchor(double cj, const WarpRenderParams& p) {
		return (cj + p.offsetOutputFrames) * p.speed;
	}

	// find the source-offset adjustment (in source frames, within +/- delta) that best lines
	// this grain up with the previous one, reducing the phasiness plain overlap-add causes on
	// pitched material. a small center bias makes 0 win on silence so we never chase noise
	double WsolaSearch(const float* samples, size_t numSamples, int clipChannels,
					   double cj, double cjPrev, double anchorJ, double prevPlacedAnchor,
					   const ModeConfig& cfg, const WarpRenderParams& p) {
		double delta = cfg.hop * p.pitchRead * 0.5; // up to half a hop of source content
		if (delta < 1.0)
			return 0.0;

		// the two grains overlap on the output grid between these frames
		double regionStart = cj - cfg.halfLen;
		double regionEnd = cjPrev + cfg.halfLen;
		double regionLen = regionEnd - regionStart;
		if (regionLen <= 1.0)
			return 0.0;
		double stride = regionLen / (double)kCorrTaps;

		double bestScore = -2.0;
		double bestAdjust = 0.0;
		for (int ci = 0; ci < kWsolaCandidates; ++ci) {
			double s = -delta + (2.0 * delta) * ((double)ci / (double)(kWsolaCandidates - 1));
			double corr = 0.0, e1 = 0.0, e2 = 0.0;
			for (int t = 0; t < kCorrTaps; ++t) {
				double outPos = regionStart + ((double)t + 0.5) * stride;
				double sJ = ReadSampleLinear(samples, numSamples, clipChannels, 0, anchorJ + s + (outPos - cj) * p.pitchRead);
				double sP = ReadSampleLinear(samples, numSamples, clipChannels, 0, prevPlacedAnchor + (outPos - cjPrev) * p.pitchRead);
				corr += sJ * sP;
				e1 += sJ * sJ;
				e2 += sP * sP;
			}
			double ncc = corr / std::sqrt(e1 * e2 + 1e-9);
			double score = ncc - 1e-3 * std::abs(s) / delta; // tie-break toward no shift
			if (score > bestScore) {
				bestScore = score;
				bestAdjust = s;
			}
		}
		return bestAdjust;
	}

	// renders output frames [a, a+n) for one clip, mixing into dest (n frames, destChannels
	// wide). a guard sample at frame a-1 is rendered too, so ComplexPro's one-zero formant
	// filter has a real previous input and introduces no click at tile boundaries
	void RenderTile(const float* samples, size_t numSamples, int clipChannels,
					int64_t a, int n, int destChannels, float* dest,
					const WarpRenderParams& p, const ModeConfig& cfg) {
		// buffer index bi in [0, n] maps to output frame (a - 1 + bi); bi 0 is the guard
		float acc[(kMaxTile + 1) * 2];
		double wgt[kMaxTile + 1];
		int bufFrames = n + 1;
		for (int i = 0; i < bufFrames * destChannels; ++i)
			acc[i] = 0.0f;
		for (int i = 0; i < bufFrames; ++i)
			wgt[i] = 0.0;

		int64_t loFrame = a - 1;
		int64_t hiFrame = a + n - 1;

		// grain indices whose window can touch [loFrame, hiFrame]
		int64_t jLo = (int64_t)std::floor(((double)loFrame - cfg.halfLen) / cfg.hop);
		int64_t jHi = (int64_t)std::floor(((double)hiFrame + cfg.halfLen) / cfg.hop);

		// per-grain source offset (WSOLA or fluctuation), indexed by (j - jStart)
		int64_t jStart = jLo;
		double adjust[kMaxGrains];
		int grainCount = (int)(jHi - jStart + 1);
		if (grainCount < 1)
			return;
		if (grainCount > kMaxGrains)
			grainCount = kMaxGrains; // safety clamp; clamps on grain size keep this unreached

		if (cfg.useWsola) {
			// align each grain to the previous grain's *unadjusted* anchor. this deliberately
			// does not chain grain-to-grain: a grain's offset is then a pure function of the
			// source and its own index, so whichever block renders it computes the identical
			// offset. that keeps the engine position-addressable (bit-exact regardless of how
			// the caller splits the range) -- a recursive chain drifts with block phase instead
			for (int gi = 0; gi < grainCount; ++gi) {
				int64_t j = jStart + gi;
				double cj = (double)j * cfg.hop;
				double cjPrev = (double)(j - 1) * cfg.hop;
				double anchorJ = GrainAnchor(cj, p);
				double prevAnchor = GrainAnchor(cjPrev, p);
				adjust[gi] = WsolaSearch(samples, numSamples, clipChannels, cj, cjPrev, anchorJ, prevAnchor, cfg, p);
			}
		} else if (cfg.useFluct) {
			double maxJitter = cfg.hop * p.pitchRead * std::clamp(p.fluctuation, 0.0, 1.0);
			for (int gi = 0; gi < grainCount; ++gi)
				adjust[gi] = GrainJitter(jStart + gi) * maxJitter;
		} else {
			for (int gi = 0; gi < grainCount; ++gi)
				adjust[gi] = 0.0;
		}

		// lay down the grains
		for (int gi = 0; gi < grainCount; ++gi) {
			int64_t j = jStart + gi;
			double cj = (double)j * cfg.hop;
			double anchor = GrainAnchor(cj, p) + adjust[gi];

			int64_t kStart = std::max(loFrame, (int64_t)std::ceil(cj - cfg.halfLen));
			int64_t kEnd = std::min(hiFrame, (int64_t)std::floor(cj + cfg.halfLen));
			for (int64_t k = kStart; k <= kEnd; ++k) {
				double local = (double)k - cj;
				double w = GrainWindow(local, cfg.halfLen, cfg.rampLen);
				if (w <= 0.0)
					continue;
				double srcPos = anchor + local * p.pitchRead;
				int bi = (int)(k - loFrame);
				for (int c = 0; c < destChannels; ++c) {
					int sc = c % clipChannels;
					acc[bi * destChannels + c] += (float)w * ReadSampleLinear(samples, numSamples, clipChannels, sc, srcPos);
				}
				wgt[bi] += w;
			}
		}

		// normalize by the summed window so the plateau/overlap and clip edges stay unity gain
		for (int bi = 0; bi < bufFrames; ++bi) {
			if (wgt[bi] > 1e-6) {
				double inv = 1.0 / wgt[bi];
				for (int c = 0; c < destChannels; ++c)
					acc[bi * destChannels + c] *= (float)inv;
			}
		}

		// ComplexPro: a one-zero tilt that counteracts the formant shift a pitch change causes
		// (pitch up brightens/thins, so darken, and vice-versa). stateless: it reads the guard
		// sample, never carrying state across blocks. crude but honest, not FFT formants
		double kco = 0.0;
		if (cfg.useFormants && p.pitchRatio > 0.0) {
			kco = std::clamp(0.4 * std::clamp(p.formants, 0.0, 1.0) * std::log2(p.pitchRatio), -0.9, 0.9);
		}

		for (int i = 0; i < n; ++i) {
			int bi = i + 1; // output frame a+i
			for (int c = 0; c < destChannels; ++c) {
				float x = acc[bi * destChannels + c];
				float y = x;
				if (kco != 0.0) {
					float xm1 = acc[(bi - 1) * destChannels + c];
					y = (float)(1.0 - kco) * x + (float)kco * xm1;
				}
				dest[i * destChannels + c] += y;
			}
		}
	}

} // namespace

void RenderWarpedBlock(const float* samples, size_t numSamples, int clipChannels,
					   int64_t outStartFrame, int count, int destChannels,
					   float* dest, const WarpRenderParams& params) {
	if (!samples || numSamples == 0 || clipChannels <= 0 || count <= 0 || destChannels <= 0)
		return;
	if (destChannels > 2)
		destChannels = 2; // engine scratch is sized for stereo, which is all the DAW mixes

	ModeConfig cfg = ResolveMode(params);
	if (cfg.grainOut <= 0.0 || cfg.hop <= 0.0)
		return;

	// tile the request so scratch stays bounded; each tile is independent and deterministic
	int done = 0;
	while (done < count) {
		int n = std::min(kMaxTile, count - done);
		RenderTile(samples, numSamples, clipChannels, outStartFrame + done, n, destChannels,
				   dest + done * destChannels, params, cfg);
		done += n;
	}
}
