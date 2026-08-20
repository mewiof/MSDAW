#include "PrecompHeader.h"
#include "EQEightProcessor.h"
#include "Parameters/KnobParameter.h"
#include "Parameters/SliderParameter.h"
#include "Parameters/ToggleParameter.h"
#include "ProcessorFactory.h"
#include "Theme.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <sstream>
#include <string>

REGISTER_PROCESSOR(EQEightProcessor, "EQEight", false)

namespace {

	constexpr double kPi = 3.14159265358979323846;

	// the graph's frequency axis, and the span every band's frequency covers
	constexpr double kMinFrequency = 10.0;
	constexpr double kMaxFrequency = 22000.0;

	// the widest boost or cut a band can reach, and the point Adaptive Q calls "full"
	constexpr float kMaxBandGainDb = 15.0f;

	// vertical gap between the rows of the globals column, tighter than the default
	constexpr int kGlobalsRowSpacing = 2;

	const char* kFilterTypeNames[(int)EQFilterType::Count] = {
		"Low Cut 48 dB",
		"Low Cut 12 dB",
		"Low Shelf",
		"Bell",
		"Notch",
		"High Shelf",
		"High Cut 12 dB",
		"High Cut 48 dB"};

	const char* kChannelModeNames[(int)EQChannelMode::Count] = {"Stereo", "L/R", "M/S"};

	// the two halves each split mode edits, in Edit's own words
	const char* kSetNames[(int)EQChannelMode::Count][2] = {
		{"A", "B"},
		{"L", "R"},
		{"M", "S"}};

	// section Q's of an eighth-order Butterworth, which is what a 48 dB/oct cut is.
	// the user's Q only scales the last (most resonant) section, so turning it up
	// gives the corner a resonant peak instead of detuning the whole cascade
	const double kButterworthQ[4] = {0.50979558, 0.60134489, 0.89997622, 2.56291545};

	double DbToLinear(double db) {
		return std::pow(10.0, db / 20.0);
	}

	double LinearToDb(double linear) {
		return 20.0 * std::log10(std::max(linear, 1.0e-9));
	}

	// center frequency of one of the analyzer's log-spaced display points. the mapping,
	// the drawing and the headless readout all have to agree on this or the spectrum
	// lands next to the curve it is supposed to sit under
	double AnalyzerPointFrequency(int index, int count) {
		const double ratio = std::log(kMaxFrequency / kMinFrequency);
		return kMinFrequency * std::exp(ratio * ((double)index + 0.5) / (double)count);
	}

	bool TypeUsesGain(EQFilterType type) {
		return type == EQFilterType::LowShelf || type == EQFilterType::Bell || type == EQFilterType::HighShelf;
	}

	// the miniature response curve on a band's type button, and beside each entry of
	// its type menu. every shape is drawn in the same box so the eight read as a set
	void DrawFilterIcon(ImDrawList* drawList, EQFilterType type, const ImVec2& pos, const ImVec2& size, ImU32 color) {
		const float thickness = 1.5f;
		const float left = pos.x + 1.0f;
		const float right = pos.x + size.x - 1.0f;
		const float top = pos.y + 2.0f;
		const float bottom = pos.y + size.y - 2.0f;
		const float middle = (top + bottom) * 0.5f;
		const float width = right - left;

		// a cut's knee sits further along the box the steeper it is, so 48 and 12 read
		// apart at a glance instead of needing the label
		const float steep = (type == EQFilterType::LowCut48 || type == EQFilterType::HighCut48) ? 0.18f : 0.42f;

		switch (type) {
		case EQFilterType::LowCut48:
		case EQFilterType::LowCut12:
			drawList->AddBezierCubic(
				ImVec2(left, bottom),
				ImVec2(left + width * steep, bottom),
				ImVec2(left + width * steep, middle - (bottom - middle) * 0.6f),
				ImVec2(right, middle - (bottom - middle) * 0.6f),
				color, thickness);
			break;
		case EQFilterType::HighCut12:
		case EQFilterType::HighCut48:
			drawList->AddBezierCubic(
				ImVec2(left, middle - (bottom - middle) * 0.6f),
				ImVec2(right - width * steep, middle - (bottom - middle) * 0.6f),
				ImVec2(right - width * steep, bottom),
				ImVec2(right, bottom),
				color, thickness);
			break;
		case EQFilterType::LowShelf:
			drawList->AddBezierCubic(
				ImVec2(left, top),
				ImVec2(left + width * 0.4f, top),
				ImVec2(left + width * 0.5f, middle),
				ImVec2(right, middle),
				color, thickness);
			break;
		case EQFilterType::HighShelf:
			drawList->AddBezierCubic(
				ImVec2(left, middle),
				ImVec2(left + width * 0.5f, middle),
				ImVec2(right - width * 0.4f, top),
				ImVec2(right, top),
				color, thickness);
			break;
		case EQFilterType::Bell:
			drawList->AddBezierCubic(
				ImVec2(left, middle),
				ImVec2(left + width * 0.3f, middle),
				ImVec2(left + width * 0.35f, top),
				ImVec2(left + width * 0.5f, top),
				color, thickness);
			drawList->AddBezierCubic(
				ImVec2(left + width * 0.5f, top),
				ImVec2(left + width * 0.65f, top),
				ImVec2(right - width * 0.3f, middle),
				ImVec2(right, middle),
				color, thickness);
			break;
		case EQFilterType::Notch:
			drawList->AddBezierCubic(
				ImVec2(left, middle),
				ImVec2(left + width * 0.32f, middle),
				ImVec2(left + width * 0.42f, bottom),
				ImVec2(left + width * 0.5f, bottom),
				color, thickness);
			drawList->AddBezierCubic(
				ImVec2(left + width * 0.5f, bottom),
				ImVec2(left + width * 0.58f, bottom),
				ImVec2(right - width * 0.32f, middle),
				ImVec2(right, middle),
				color, thickness);
			break;
		default:
			break;
		}
	}

} // namespace

// ================================================================
// HALFBAND RESAMPLER
// ================================================================

const double* EQHalfband::Kernel() {
	// a windowed-sinc halfband: every even offset from the center is already zero, so
	// only the sixteen even *indices* of the 31-tap kernel survive, and those are
	// symmetric. the odd indices collapse to the single center tap, which is why the
	// other polyphase phase below is a plain delay
	static const std::array<double, EQHalfband::kTaps> kernel = [] {
		std::array<double, EQHalfband::kTaps> taps{};
		const int length = 31;
		const int center = 15;
		double sum = 0.0;

		for (int j = 0; j < EQHalfband::kTaps; ++j) {
			const int index = j * 2;
			const double offset = (double)(index - center); // always odd, never the sinc's hole
			const double sinc = std::sin(kPi * offset * 0.5) / (kPi * offset);
			const double phase = 2.0 * kPi * index / (double)(length - 1);
			const double window = 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
			taps[j] = sinc * window;
			sum += taps[j];
		}

		// the delay phase contributes exactly 0.5 of the unity DC gain, so the taps
		// have to make up the other half no matter what the window did to them
		const double scale = 0.5 / sum;
		for (double& tap : taps)
			tap *= scale;
		return taps;
	}();

	return kernel.data();
}

void EQHalfband::Reset() {
	filtered.fill(0.0);
	delayed.fill(0.0);
	writeIndex = 0;
}

void EQHalfband::Up(double in, double& out0, double& out1) {
	const double* kernel = Kernel();
	filtered[writeIndex] = in;

	double sum = 0.0;
	for (int j = 0; j < kTaps / 2; ++j) {
		const double near = filtered[(writeIndex - j) & (kHistory - 1)];
		const double far = filtered[(writeIndex - (kTaps - 1 - j)) & (kHistory - 1)];
		sum += kernel[j] * (near + far);
	}

	// x2 because zero-stuffing halved the energy the kernel now has to restore
	out0 = sum * 2.0;
	out1 = filtered[(writeIndex - 7) & (kHistory - 1)];
	writeIndex = (writeIndex + 1) & (kHistory - 1);
}

double EQHalfband::Down(double in0, double in1) {
	const double* kernel = Kernel();
	filtered[writeIndex] = in0; // even-indexed samples of the 2x stream
	delayed[writeIndex] = in1;	// odd-indexed ones

	double sum = 0.0;
	for (int j = 0; j < kTaps / 2; ++j) {
		const double near = filtered[(writeIndex - j) & (kHistory - 1)];
		const double far = filtered[(writeIndex - (kTaps - 1 - j)) & (kHistory - 1)];
		sum += kernel[j] * (near + far);
	}

	const double center = delayed[(writeIndex - 8) & (kHistory - 1)];
	writeIndex = (writeIndex + 1) & (kHistory - 1);
	return sum + 0.5 * center;
}

// ================================================================
// LIFECYCLE
// ================================================================

EQEightProcessor::EQEightProcessor() {
	// the default curve is dead flat: the two cut bands are parked off at the edges
	// and everything else sits at 0 dB, where a bell and a shelf are transparent. an
	// EQ that colors the signal the moment it is dropped on a track is a bug
	struct BandDefault {
		EQFilterType type;
		float frequency;
		bool active;
	};
	static const BandDefault kBandDefaults[kNumBands] = {
		{EQFilterType::LowCut48, 30.0f, false},
		{EQFilterType::LowShelf, 100.0f, true},
		{EQFilterType::Bell, 250.0f, true},
		{EQFilterType::Bell, 500.0f, true},
		{EQFilterType::Bell, 1000.0f, true},
		{EQFilterType::Bell, 2500.0f, true},
		{EQFilterType::HighShelf, 5000.0f, true},
		{EQFilterType::HighCut48, 15000.0f, false}};

	for (int set = 0; set < kNumSets; ++set) {
		for (int band = 0; band < kNumBands; ++band) {
			const BandDefault& preset = kBandDefaults[band];
			// the suffix is what tells two otherwise identical bands apart in an
			// automation lane, and automation is rebound by parameter name, so it is
			// as frozen as the serialization format is
			const std::string suffix = " " + std::to_string(band + 1) + (set == 0 ? "A" : "B");
			BandParams& params = mBands[set][band];

			params.pFrequency = AddParameter(std::make_unique<KnobParameter>(
				"Freq" + suffix, preset.frequency, (float)kMinFrequency, (float)kMaxFrequency, ImGuiKnobVariant_Hertz));
			params.pGain = AddParameter(std::make_unique<KnobParameter>(
				"Gain" + suffix, 0.0f, -kMaxBandGainDb, kMaxBandGainDb, ImGuiKnobVariant_DecibelBipolar));
			params.pQ = AddParameter(std::make_unique<KnobParameter>(
				"Q" + suffix, 0.71f, 0.1f, 18.0f));
			params.pType = AddParameter(std::make_unique<SliderParameter>(
				"Type" + suffix, (float)(int)preset.type, 0.0f, (float)((int)EQFilterType::Count - 1)));
			params.pActive = AddParameter(std::make_unique<ToggleParameter>(
				"On" + suffix, preset.active ? 1.0f : 0.0f, 0.0f, 1.0f));
		}
	}

	pMode = AddParameter(std::make_unique<SliderParameter>("Mode", 0.0f, 0.0f, (float)((int)EQChannelMode::Count - 1)));
	pAdaptQ = AddParameter(std::make_unique<ToggleParameter>("Adapt Q", 0.0f, 0.0f, 1.0f));
	pScale = AddParameter(std::make_unique<KnobParameter>("Scale", 100.0f, 0.0f, 200.0f, ImGuiKnobVariant_Percent));
	pOutputGain = AddParameter(std::make_unique<KnobParameter>("Gain", 0.0f, -12.0f, 12.0f, ImGuiKnobVariant_DecibelBipolar));

	mAnalyzerRing.assign(kRingSize, 0.0f);
	mAnalyzerDb.assign(kAnalyzerBins, -120.0f);
}

void EQEightProcessor::PrepareToPlay(double sampleRate) {
	mSampleRate = sampleRate > 1.0 ? sampleRate : 48000.0;
	mProcessRate = mSampleRate * (mOversampleActive ? 2.0 : 1.0);
	mForceRedesign = true;

	// touch the shared halfband kernel from here, which runs on the UI thread, so the
	// audio thread never has to run the one-time initialization itself
	EQHalfband::Kernel();

	ResetFilterState();
}

void EQEightProcessor::Reset() {
	ResetFilterState();
	std::fill(mAnalyzerRing.begin(), mAnalyzerRing.end(), 0.0f);
}

void EQEightProcessor::ResetFilterState() {
	for (BandStates& channel : mStates)
		channel = BandStates{};
	for (std::array<EQBiquadState, 2>& channel : mAuditionStates)
		channel = std::array<EQBiquadState, 2>{};
	for (EQHalfband& filter : mUpsamplers)
		filter.Reset();
	for (EQHalfband& filter : mDownsamplers)
		filter.Reset();
}

void EQEightProcessor::EnsureChannelState(int numChannels) {
	if ((int)mStates.size() == numChannels)
		return;

	// NOTE: this allocates on the audio thread, but only on the first block and again
	// if the channel count ever changes under us - both one-off events at stream start
	mStates.assign(numChannels, BandStates{});
	mAuditionStates.assign(numChannels, std::array<EQBiquadState, 2>{});
	mUpsamplers.assign(numChannels, EQHalfband{});
	mDownsamplers.assign(numChannels, EQHalfband{});
}

EQChannelMode EQEightProcessor::GetChannelMode() const {
	return (EQChannelMode)std::clamp((int)std::lround(pMode->value), 0, (int)EQChannelMode::Count - 1);
}

// ================================================================
// COEFFICIENTS
// ================================================================

void EQEightProcessor::ApplyGlobalShaping(int type, double& gainDb, double& q) const {
	const EQFilterType shape = (EQFilterType)type;

	if (TypeUsesGain(shape)) {
		// Scale rides every boost and cut at once, so a whole curve can be dialed back
		// without touching eight knobs
		gainDb *= (double)pScale->value / 100.0;
	} else {
		gainDb = 0.0; // a cut or a notch has no gain to give
	}

	// Adaptive Q narrows a bell as it is pushed, the way a passive analog EQ does:
	// gentle moves stay broad and musical, extreme ones turn surgical. shelves and
	// cuts have no comparable width to trade, so they are left alone
	if (pAdaptQ->value > 0.5f && shape == EQFilterType::Bell) {
		const double amount = std::min(std::abs(gainDb) / (double)kMaxBandGainDb, 1.0);
		q *= 1.0 + amount;
	}
}

int EQEightProcessor::BuildSections(int type, double frequency, double gainDb, double q,
									double rate, EQBiquad* sections) {
	if (rate < 1.0)
		rate = 48000.0;

	// a corner at or above Nyquist has no meaning. parking it just below keeps a high
	// cut dragged off the top of the axis at 44.1 kHz merely inaudible, rather than a
	// source of garbage coefficients
	const double nyquist = rate * 0.5;
	frequency = std::clamp(frequency, 1.0, nyquist * 0.995);
	q = std::max(q, 0.05);

	const double w0 = 2.0 * kPi * frequency / rate;
	const double cosW0 = std::cos(w0);
	const double sinW0 = std::sin(w0);

	// every shape below is the RBJ cookbook form, normalized so a0 falls out
	auto normalize = [](EQBiquad& section, double a0) {
		if (std::abs(a0) < 1.0e-12)
			a0 = 1.0;
		section.b0 /= a0;
		section.b1 /= a0;
		section.b2 /= a0;
		section.a1 /= a0;
		section.a2 /= a0;
	};

	auto makeCut = [&](bool highPass, double sectionQ) {
		const double alpha = sinW0 / (2.0 * std::max(sectionQ, 0.05));
		EQBiquad section;
		if (highPass) {
			section.b0 = (1.0 + cosW0) * 0.5;
			section.b1 = -(1.0 + cosW0);
			section.b2 = (1.0 + cosW0) * 0.5;
		} else {
			section.b0 = (1.0 - cosW0) * 0.5;
			section.b1 = 1.0 - cosW0;
			section.b2 = (1.0 - cosW0) * 0.5;
		}
		section.a1 = -2.0 * cosW0;
		section.a2 = 1.0 - alpha;
		normalize(section, 1.0 + alpha);
		return section;
	};

	const double alpha = sinW0 / (2.0 * q);
	const double a = DbToLinear(gainDb * 0.5); // sqrt of the linear gain, per the cookbook
	const double sqrtA = std::sqrt(a);

	switch ((EQFilterType)type) {
	case EQFilterType::LowCut12:
		sections[0] = makeCut(true, q);
		return 1;
	case EQFilterType::HighCut12:
		sections[0] = makeCut(false, q);
		return 1;
	case EQFilterType::LowCut48:
	case EQFilterType::HighCut48: {
		const bool highPass = (EQFilterType)type == EQFilterType::LowCut48;
		for (int i = 0; i < 4; ++i) {
			// only the last section answers to the Q knob; the other three stay
			// Butterworth so the slope below the corner is exactly 48 dB/oct
			const double sectionQ = (i == 3) ? kButterworthQ[i] * (q / 0.7071) : kButterworthQ[i];
			sections[i] = makeCut(highPass, sectionQ);
		}
		return 4;
	}
	case EQFilterType::Notch: {
		EQBiquad section;
		section.b0 = 1.0;
		section.b1 = -2.0 * cosW0;
		section.b2 = 1.0;
		section.a1 = -2.0 * cosW0;
		section.a2 = 1.0 - alpha;
		normalize(section, 1.0 + alpha);
		sections[0] = section;
		return 1;
	}
	case EQFilterType::Bell: {
		EQBiquad section;
		section.b0 = 1.0 + alpha * a;
		section.b1 = -2.0 * cosW0;
		section.b2 = 1.0 - alpha * a;
		section.a1 = -2.0 * cosW0;
		section.a2 = 1.0 - alpha / a;
		normalize(section, 1.0 + alpha / a);
		sections[0] = section;
		return 1;
	}
	case EQFilterType::LowShelf: {
		EQBiquad section;
		section.b0 = a * ((a + 1.0) - (a - 1.0) * cosW0 + 2.0 * sqrtA * alpha);
		section.b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosW0);
		section.b2 = a * ((a + 1.0) - (a - 1.0) * cosW0 - 2.0 * sqrtA * alpha);
		section.a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosW0);
		section.a2 = (a + 1.0) + (a - 1.0) * cosW0 - 2.0 * sqrtA * alpha;
		normalize(section, (a + 1.0) + (a - 1.0) * cosW0 + 2.0 * sqrtA * alpha);
		sections[0] = section;
		return 1;
	}
	case EQFilterType::HighShelf: {
		EQBiquad section;
		section.b0 = a * ((a + 1.0) + (a - 1.0) * cosW0 + 2.0 * sqrtA * alpha);
		section.b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW0);
		section.b2 = a * ((a + 1.0) + (a - 1.0) * cosW0 - 2.0 * sqrtA * alpha);
		section.a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosW0);
		section.a2 = (a + 1.0) - (a - 1.0) * cosW0 - 2.0 * sqrtA * alpha;
		normalize(section, (a + 1.0) - (a - 1.0) * cosW0 + 2.0 * sqrtA * alpha);
		sections[0] = section;
		return 1;
	}
	default:
		break;
	}

	sections[0] = EQBiquad{};
	return 1;
}

void EQEightProcessor::UpdateCoefficients(int numFrames) {
	// one pole per block. 20 ms is slow enough to hide the step when a coefficient set
	// is swapped out and fast enough that a knob still feels immediate
	const double blockSeconds = (double)numFrames / std::max(mSampleRate, 1.0);
	const double smoothing = std::clamp(1.0 - std::exp(-blockSeconds / 0.02), 0.0, 1.0);

	for (int set = 0; set < kNumSets; ++set) {
		for (int band = 0; band < kNumBands; ++band) {
			const BandParams& params = mBands[set][band];
			BandRuntime& runtime = mRuntime[set][band];

			const int type = std::clamp((int)std::lround(params.pType->value), 0, (int)EQFilterType::Count - 1);
			const bool active = params.pActive->value > 0.5f;
			double frequency = std::max((double)params.pFrequency->value, 1.0);
			double gain = (double)params.pGain->value;
			double q = (double)params.pQ->value;
			ApplyGlobalShaping(type, gain, q);

			const double previousFrequency = runtime.frequency;
			const double previousGain = runtime.gain;
			const double previousQ = runtime.q;

			if (!runtime.primed) {
				// the first block after a load or an insert must not glide up from
				// whatever the runtime happened to hold
				runtime.frequency = frequency;
				runtime.gain = gain;
				runtime.q = q;
				runtime.primed = true;
			} else {
				// frequency glides in octaves so a sweep moves evenly across the axis
				const double ratio = frequency / std::max(runtime.frequency, 1.0);
				runtime.frequency *= std::exp(std::log(ratio) * smoothing);
				runtime.gain += (gain - runtime.gain) * smoothing;
				runtime.q += (q - runtime.q) * smoothing;

				// snap once the rest of the distance is inaudible: a one-pole never
				// actually arrives, and without this every block redesigns forever
				if (std::abs(runtime.frequency - frequency) < frequency * 1.0e-4)
					runtime.frequency = frequency;
				if (std::abs(runtime.gain - gain) < 1.0e-4)
					runtime.gain = gain;
				if (std::abs(runtime.q - q) < 1.0e-4)
					runtime.q = q;
			}

			const bool moved = mForceRedesign || runtime.type != type || runtime.active != active ||
							   runtime.frequency != previousFrequency || runtime.gain != previousGain ||
							   runtime.q != previousQ;
			runtime.type = type;
			runtime.active = active;

			if (moved) {
				runtime.numSections = active
										  ? BuildSections(type, runtime.frequency, runtime.gain, runtime.q,
														  mProcessRate, runtime.sections.data())
										  : 0;
			}
		}
	}

	mForceRedesign = false;
}

void EQEightProcessor::UpdateAuditionSection() {
	const int band = std::clamp(mAuditionBand.load(std::memory_order_relaxed), 0, kNumBands - 1);
	const int set = std::clamp(mAuditionSet.load(std::memory_order_relaxed), 0, kNumSets - 1);
	const BandParams& params = mBands[set][band];

	// a constant-peak-gain bandpass at the band's own frequency. two of these are
	// cascaded in the loop below, because one at a musical Q is too wide to tell you
	// anything about what you are pointing at
	const double rate = std::max(mProcessRate, 1.0);
	const double frequency = std::clamp((double)params.pFrequency->value, 1.0, rate * 0.5 * 0.995);
	const double q = std::max((double)params.pQ->value, 2.0);
	const double w0 = 2.0 * kPi * frequency / rate;
	const double alpha = std::sin(w0) / (2.0 * q);
	const double a0 = 1.0 + alpha;

	mAuditionSection.b0 = alpha / a0;
	mAuditionSection.b1 = 0.0;
	mAuditionSection.b2 = -alpha / a0;
	mAuditionSection.a1 = -2.0 * std::cos(w0) / a0;
	mAuditionSection.a2 = (1.0 - alpha) / a0;
}

// ================================================================
// PROCESS
// ================================================================

double EQEightProcessor::ProcessSample(int channel, int setIndex, double sample) {
	BandStates& states = mStates[channel];

	for (int band = 0; band < kNumBands; ++band) {
		const BandRuntime& runtime = mRuntime[setIndex][band];
		for (int index = 0; index < runtime.numSections; ++index) {
			const EQBiquad& coefficients = runtime.sections[index];
			EQBiquadState& state = states[band][index];

			const double out = coefficients.b0 * sample + state.z1;
			state.z1 = coefficients.b1 * sample - coefficients.a1 * out + state.z2;
			state.z2 = coefficients.b2 * sample - coefficients.a2 * out;
			sample = out;
		}
	}

	return sample;
}

double EQEightProcessor::RunChannel(int channel, int setIndex, double sample) {
	if (!mOversampleActive)
		return ProcessSample(channel, setIndex, sample);

	double first = 0.0;
	double second = 0.0;
	mUpsamplers[channel].Up(sample, first, second);
	first = ProcessSample(channel, setIndex, first);
	second = ProcessSample(channel, setIndex, second);
	return mDownsamplers[channel].Down(first, second);
}

void EQEightProcessor::Process(float* buffer, int numFrames, int numChannels,
							   std::vector<MIDIMessage>& mIDIMessages,
							   const ProcessContext& context) {
	(void)mIDIMessages;
	(void)context;

	if (!buffer || numFrames <= 0 || numChannels <= 0)
		return;

	EnsureChannelState(numChannels);

	const bool oversample = mOversample.load(std::memory_order_relaxed);
	if (oversample != mOversampleActive) {
		mOversampleActive = oversample;
		mProcessRate = mSampleRate * (oversample ? 2.0 : 1.0);
		mForceRedesign = true;
		ResetFilterState();
	}

	const int mode = std::clamp((int)std::lround(pMode->value), 0, (int)EQChannelMode::Count - 1);
	if (mode != mActiveMode) {
		// a channel that just changed which set it runs through is carrying the wrong
		// filter memory, and an M/S switch changes what that memory even means
		mActiveMode = mode;
		ResetFilterState();
	}

	UpdateCoefficients(numFrames);

	const bool audition = mAudition.load(std::memory_order_relaxed);
	if (audition)
		UpdateAuditionSection();

	const double outputGain = DbToLinear((double)pOutputGain->value);
	const bool midSide = mode == (int)EQChannelMode::MidSide && numChannels >= 2;
	const bool leftRight = mode == (int)EQChannelMode::LeftRight && numChannels >= 2;

	for (int i = 0; i < numFrames; ++i) {
		float* frame = buffer + (size_t)i * (size_t)numChannels;

		if (audition) {
			// monitoring one band: the chain is skipped entirely and what comes out is
			// the slice of the input that band is pointing at
			for (int c = 0; c < numChannels; ++c) {
				double sample = frame[c];
				for (int index = 0; index < 2; ++index) {
					EQBiquadState& state = mAuditionStates[c][index];
					const double out = mAuditionSection.b0 * sample + state.z1;
					state.z1 = mAuditionSection.b1 * sample - mAuditionSection.a1 * out + state.z2;
					state.z2 = mAuditionSection.b2 * sample - mAuditionSection.a2 * out;
					sample = out;
				}
				frame[c] = (float)(sample * outputGain);
			}
		} else if (midSide) {
			const double left = frame[0];
			const double right = frame[1];
			const double mid = RunChannel(0, 0, (left + right) * 0.5);
			const double side = RunChannel(1, 1, (left - right) * 0.5);
			frame[0] = (float)((mid + side) * outputGain);
			frame[1] = (float)((mid - side) * outputGain);

			// anything past the first pair is not part of the stereo image, so it runs
			// through set A untouched by the encode
			for (int c = 2; c < numChannels; ++c)
				frame[c] = (float)(RunChannel(c, 0, frame[c]) * outputGain);
		} else {
			for (int c = 0; c < numChannels; ++c) {
				const int set = (leftRight && c == 1) ? 1 : 0;
				frame[c] = (float)(RunChannel(c, set, frame[c]) * outputGain);
			}
		}

		// the analyzer listens to the output, like the original: what it draws is the
		// spectrum the curve above it just produced, not the one that arrived
		float monoSum = 0.0f;
		for (int c = 0; c < numChannels; ++c)
			monoSum += frame[c];
		PushAnalyzerSample(monoSum / (float)numChannels);
	}

	// a biquad decaying into silence spends a long stretch in denormal territory,
	// where some CPUs fall off a cliff. one sweep per block keeps it out of there
	for (BandStates& channel : mStates) {
		for (std::array<EQBiquadState, kMaxSections>& band : channel) {
			for (EQBiquadState& state : band) {
				if (std::abs(state.z1) < 1.0e-25)
					state.z1 = 0.0;
				if (std::abs(state.z2) < 1.0e-25)
					state.z2 = 0.0;
			}
		}
	}
}

// ================================================================
// RESPONSE
// ================================================================

void EQEightProcessor::GetResponseDb(int setIndex, int bandIndex, const double* frequencies,
									 float* outputDb, int count) const {
	if (!frequencies || !outputDb || count <= 0)
		return;

	setIndex = std::clamp(setIndex, 0, kNumSets - 1);

	// the curve answers for the base rate even while oversampling: what it promises is
	// the response the listener hears, and the decimator hides the 2x domain
	const double rate = mSampleRate > 1.0 ? mSampleRate : 48000.0;

	std::array<EQBiquad, kNumBands * kMaxSections> sections;
	int sectionCount = 0;

	for (int band = 0; band < kNumBands; ++band) {
		if (bandIndex >= 0 && band != bandIndex)
			continue;

		const BandParams& params = mBands[setIndex][band];
		if (params.pActive->value <= 0.5f)
			continue;

		const int type = std::clamp((int)std::lround(params.pType->value), 0, (int)EQFilterType::Count - 1);
		double gain = (double)params.pGain->value;
		double q = (double)params.pQ->value;
		ApplyGlobalShaping(type, gain, q);
		sectionCount += BuildSections(type, (double)params.pFrequency->value, gain, q, rate,
									  sections.data() + sectionCount);
	}

	// the output gain shifts the whole curve, so it belongs to the sum and to nothing
	// else - a single band's own shape is drawn where the user put it
	const double offsetDb = bandIndex < 0 ? (double)pOutputGain->value : 0.0;

	for (int i = 0; i < count; ++i) {
		const double w = 2.0 * kPi * std::clamp(frequencies[i], 0.0, rate * 0.4999) / rate;
		const std::complex<double> z1 = std::polar(1.0, -w);
		const std::complex<double> z2 = std::polar(1.0, -2.0 * w);

		double magnitudeDb = offsetDb;
		for (int s = 0; s < sectionCount; ++s) {
			const EQBiquad& coefficients = sections[s];
			const std::complex<double> numerator = coefficients.b0 + coefficients.b1 * z1 + coefficients.b2 * z2;
			const std::complex<double> denominator = 1.0 + coefficients.a1 * z1 + coefficients.a2 * z2;
			const double magnitude = std::abs(denominator) > 1.0e-12
										 ? std::abs(numerator) / std::abs(denominator)
										 : 1.0;
			magnitudeDb += LinearToDb(magnitude);
		}
		outputDb[i] = (float)magnitudeDb;
	}
}

// ================================================================
// ANALYZER
// ================================================================

void EQEightProcessor::GetAnalyzerPoint(int index, double& frequency, float& db) const {
	index = std::clamp(index, 0, kAnalyzerBins - 1);
	frequency = AnalyzerPointFrequency(index, kAnalyzerBins);
	db = mAnalyzerDb[index];
}

void EQEightProcessor::PushAnalyzerSample(float sample) {
	const int index = mAnalyzerWrite.load(std::memory_order_relaxed);
	mAnalyzerRing[index] = sample;
	mAnalyzerWrite.store((index + 1) & (kRingSize - 1), std::memory_order_relaxed);
}

void EQEightProcessor::RefreshAnalyzer() {
	// a Hann-windowed radix-2 transform of the newest samples, folded onto the graph's
	// log axis. all of it runs here on the UI thread, so the audio thread's only job is
	// to append to the ring - and a torn read there costs one frame of a stale bin
	//
	// NOTE: the scratch is function-local rather than per-instance because only the UI
	// thread ever reaches this, and it is refilled from scratch on every call
	static std::vector<double> real(kTransformSize);
	static std::vector<double> imaginary(kTransformSize);

	const int size = kTransformSize;
	const int write = mAnalyzerWrite.load(std::memory_order_relaxed);
	for (int i = 0; i < size; ++i) {
		const float sample = mAnalyzerRing[(write - size + i) & (kRingSize - 1)];
		const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * (double)i / (double)(size - 1));
		real[i] = (double)sample * window;
		imaginary[i] = 0.0;
	}

	for (int i = 1, j = 0; i < size; ++i) {
		int bit = size >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			std::swap(real[i], real[j]);
			std::swap(imaginary[i], imaginary[j]);
		}
	}

	for (int length = 2; length <= size; length <<= 1) {
		const double angle = -2.0 * kPi / (double)length;
		const double stepReal = std::cos(angle);
		const double stepImaginary = std::sin(angle);
		for (int start = 0; start < size; start += length) {
			double twiddleReal = 1.0;
			double twiddleImaginary = 0.0;
			for (int k = 0; k < length / 2; ++k) {
				const int low = start + k;
				const int high = low + length / 2;
				const double productReal = real[high] * twiddleReal - imaginary[high] * twiddleImaginary;
				const double productImaginary = real[high] * twiddleImaginary + imaginary[high] * twiddleReal;
				real[high] = real[low] - productReal;
				imaginary[high] = imaginary[low] - productImaginary;
				real[low] += productReal;
				imaginary[low] += productImaginary;

				const double nextReal = twiddleReal * stepReal - twiddleImaginary * stepImaginary;
				twiddleImaginary = twiddleReal * stepImaginary + twiddleImaginary * stepReal;
				twiddleReal = nextReal;
			}
		}
	}

	static std::vector<double> magnitude(kTransformSize / 2 + 1);
	for (int k = 0; k <= size / 2; ++k)
		magnitude[k] = std::sqrt(real[k] * real[k] + imaginary[k] * imaginary[k]);

	const double rate = mSampleRate > 1.0 ? mSampleRate : 48000.0;
	const double binHz = rate / (double)size;
	const double ratio = std::log(kMaxFrequency / kMinFrequency);

	// the log axis and the transform's linear bins disagree at both ends, in opposite
	// directions, and taking a peak over a span only answers one of them:
	//   low  - a display point is narrower than one bin, so a plain lookup repeats the
	//          same bin across dozens of points and the curve comes out in steps
	//   high - a display point spans dozens of bins, where the peak is the honest
	//          answer because a lone tone must not average itself away
	// so interpolate between neighboring bins below the crossover and peak above it
	static std::vector<float> fresh(kAnalyzerBins);
	for (int bin = 0; bin < kAnalyzerBins; ++bin) {
		const double low = kMinFrequency * std::exp(ratio * (double)bin / (double)kAnalyzerBins);
		const double high = kMinFrequency * std::exp(ratio * (double)(bin + 1) / (double)kAnalyzerBins);

		const int first = std::max((int)std::floor(low / binHz), 1);
		const int last = std::min((int)std::ceil(high / binHz), size / 2 - 1);

		// NOTE: the first few points sit below bin 1's center, where the transform has
		// nothing to say, so they all report it. that is two percent of the width at
		// the extreme left edge and no window short enough to be responsive fixes it
		double level = 0.0;
		if (last - first < 2) {
			const double exact = AnalyzerPointFrequency(bin, kAnalyzerBins) / binHz;
			const int lower = std::clamp((int)std::floor(exact), 1, size / 2 - 2);
			const double t = std::clamp(exact - (double)lower, 0.0, 1.0);
			level = magnitude[lower] * (1.0 - t) + magnitude[lower + 1] * t;
		} else {
			for (int k = first; k <= last; ++k)
				level = std::max(level, magnitude[k]);
		}

		// 4/N undoes both the transform's length and the Hann window's 0.5 coherent
		// gain, so a full-scale sine reads 0 dBFS
		fresh[bin] = (float)LinearToDb(level * 4.0 / (double)size);
	}

	for (int bin = 0; bin < kAnalyzerBins; ++bin) {
		// NOTE: no smoothing across neighboring points. it reads better on a noise
		// floor and it is a lie about every tone: a peak flanked by quiet neighbors
		// loses several dB of the level the analyzer exists to report
		//
		// instant attack, slow release: a transient shows up at its real height and
		// then falls back, instead of flickering with every frame
		float& shown = mAnalyzerDb[bin];
		shown = fresh[bin] > shown ? fresh[bin] : shown + (fresh[bin] - shown) * 0.15f;
	}
}

// ================================================================
// SERIALIZATION
// ================================================================

void EQEightProcessor::Save(std::ostream& out) {
	// written before the inherited PARAMS block: AudioProcessor::Load stops at
	// PARAMS_END and Track::Load eats exactly one line after it, so there is no room
	// for extra lines on the far side
	out << "EQ_VIEW " << mSelectedBand << " " << mEditSet << " " << (mShowAnalyzer ? 1 : 0) << " " << mGraphRangeDb << "\n";
	out << "EQ_OVERSAMPLE " << (IsOversampling() ? 1 : 0) << "\n";
	AudioProcessor::Save(out);
}

void EQEightProcessor::Load(std::istream& in) {
	std::string line;

	while (std::getline(in, line)) {
		if (line.rfind("EQ_VIEW ", 0) == 0) {
			std::stringstream stream(line.substr(8));
			int analyzer = 1;
			stream >> mSelectedBand >> mEditSet >> analyzer >> mGraphRangeDb;
			mShowAnalyzer = analyzer != 0;
		} else if (line.rfind("EQ_OVERSAMPLE ", 0) == 0) {
			std::stringstream stream(line.substr(14));
			int enabled = 0;
			stream >> enabled;
			SetOversampling(enabled != 0);
		} else {
			// anything else is the base class's territory. it tolerates the leading
			// line we just consumed, and reads on to PARAMS_END
			AudioProcessor::Load(in);
			break;
		}
	}

	mSelectedBand = std::clamp(mSelectedBand, 0, kNumBands - 1);
	mEditSet = std::clamp(mEditSet, 0, kNumSets - 1);
	mGraphRangeDb = std::clamp(mGraphRangeDb, 6.0f, 30.0f);
	mForceRedesign = true;
}

void EQEightProcessor::CopyStateFrom(const AudioProcessor& other) {
	const EQEightProcessor* source = dynamic_cast<const EQEightProcessor*>(&other);
	if (!source)
		return;

	mSelectedBand = source->mSelectedBand;
	mEditSet = source->mEditSet;
	mShowAnalyzer = source->mShowAnalyzer;
	mGraphRangeDb = source->mGraphRangeDb;
	SetOversampling(source->IsOversampling());
	mForceRedesign = true;

	// audition is deliberately not copied: it is a monitoring mode, and a duplicated
	// device that silently soloed one band would sound broken
}

// ================================================================
// UI
// ================================================================

void EQEightProcessor::AnchorHandleDrag(const BandParams& params) {
	mDragAnchorMouse = ImGui::GetIO().MousePos;
	mDragAnchorFrequency = params.pFrequency->value;
	mDragAnchorGain = params.pGain->value;
	mDragAnchorQ = params.pQ->value;
}

void EQEightProcessor::DrawGraph(const ImVec2& pos, const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImVec2 end(pos.x + size.x, pos.y + size.y);
	const ImVec2 restore = ImGui::GetCursorScreenPos();

	auto FrequencyToX = [&](double hz) {
		const double t = std::log(std::max(hz, 1.0) / kMinFrequency) / std::log(kMaxFrequency / kMinFrequency);
		return pos.x + (float)(t * size.x);
	};
	auto XToFrequency = [&](float x) {
		const double t = std::clamp((double)(x - pos.x) / (double)size.x, 0.0, 1.0);
		return kMinFrequency * std::pow(kMaxFrequency / kMinFrequency, t);
	};
	auto DbToY = [&](double db) {
		const double t = std::clamp(db / (double)mGraphRangeDb, -1.0, 1.0);
		return pos.y + size.y * 0.5f - (float)(t * size.y * 0.5);
	};

	drawList->AddRectFilled(pos, end, th.bgDeepest);
	drawList->PushClipRect(pos, end, true);

	// ---- grid ----
	for (double decade = 10.0; decade < kMaxFrequency; decade *= 10.0) {
		for (int step = 1; step <= 9; ++step) {
			const double hz = decade * step;
			if (hz < kMinFrequency || hz > kMaxFrequency)
				continue;
			const float x = FrequencyToX(hz);
			drawList->AddLine(ImVec2(x, pos.y), ImVec2(x, end.y), step == 1 ? th.gridBar : th.gridSub);
		}
	}
	for (double hz : {100.0, 1000.0, 10000.0}) {
		char label[16];
		if (hz >= 1000.0)
			snprintf(label, sizeof(label), "%gk", hz / 1000.0);
		else
			snprintf(label, sizeof(label), "%g", hz);
		drawList->AddText(ImVec2(FrequencyToX(hz) + 3.0f, end.y - ImGui::GetTextLineHeight() - 2.0f), th.textDim, label);
	}

	for (int step = 0; (double)step * 6.0 <= (double)mGraphRangeDb; ++step) {
		for (int sign = (step == 0 ? 0 : -1); sign <= 1; sign += 2) {
			const double db = (double)sign * step * 6.0;
			const float y = DbToY(db);
			drawList->AddLine(ImVec2(pos.x, y), ImVec2(end.x, y), step == 0 ? th.gridBeat : th.gridSub);
			char label[16];
			snprintf(label, sizeof(label), "%g", db);
			drawList->AddText(ImVec2(pos.x + 3.0f, y - ImGui::GetTextLineHeight() * 0.5f), th.textDim, label);
			if (step == 0)
				break;
		}
	}

	// ---- analyzer ----
	if (mShowAnalyzer) {
		RefreshAnalyzer();

		// the spectrum has its own scale: full scale at the top of the graph down to
		// -90 dBFS at the bottom, which is the range anything audible lives in
		const float floorDb = -90.0f;
		ImVec2 previous(pos.x, end.y);
		for (int bin = 0; bin < kAnalyzerBins; ++bin) {
			const double hz = AnalyzerPointFrequency(bin, kAnalyzerBins);
			const float level = std::clamp((mAnalyzerDb[bin] - floorDb) / -floorDb, 0.0f, 1.0f);
			const ImVec2 point(FrequencyToX(hz), end.y - level * size.y);
			if (bin > 0) {
				drawList->AddQuadFilled(previous, point, ImVec2(point.x, end.y), ImVec2(previous.x, end.y),
										th.spectrumFill);
				drawList->AddLine(previous, point, th.spectrumEdge, 1.0f);
			}
			previous = point;
		}
	}

	// ---- curves ----
	const int points = std::clamp((int)size.x, 32, 480);
	std::vector<double> frequencies((size_t)points);
	std::vector<float> response((size_t)points);
	for (int i = 0; i < points; ++i)
		frequencies[(size_t)i] = XToFrequency(pos.x + (float)i * size.x / (float)(points - 1));

	// the set that is not being edited stays visible, faintly, so an M/S or L/R pair
	// can be compared without switching back and forth
	if (GetActiveSetCount() > 1) {
		GetResponseDb(1 - mEditSet, -1, frequencies.data(), response.data(), points);
		for (int i = 1; i < points; ++i) {
			drawList->AddLine(ImVec2(pos.x + (float)(i - 1) * size.x / (float)(points - 1), DbToY(response[(size_t)i - 1])),
							  ImVec2(pos.x + (float)i * size.x / (float)(points - 1), DbToY(response[(size_t)i])),
							  Theme::WithAlpha(th.graphCurveCool, 70), 1.0f);
		}
	}

	// the band being dragged shows its own shape under the sum, which is the only way
	// to see what a single filter is doing once eight of them overlap
	if (mDraggingBand >= 0) {
		GetResponseDb(mEditSet, mDraggingBand, frequencies.data(), response.data(), points);
		for (int i = 1; i < points; ++i) {
			drawList->AddLine(ImVec2(pos.x + (float)(i - 1) * size.x / (float)(points - 1), DbToY(response[(size_t)i - 1])),
							  ImVec2(pos.x + (float)i * size.x / (float)(points - 1), DbToY(response[(size_t)i])),
							  Theme::WithAlpha(th.accent, 110), 1.0f);
		}
	}

	GetResponseDb(mEditSet, -1, frequencies.data(), response.data(), points);
	for (int i = 1; i < points; ++i) {
		const ImVec2 from(pos.x + (float)(i - 1) * size.x / (float)(points - 1), DbToY(response[(size_t)i - 1]));
		const ImVec2 to(pos.x + (float)i * size.x / (float)(points - 1), DbToY(response[(size_t)i]));
		drawList->AddQuadFilled(from, to, ImVec2(to.x, DbToY(0.0)), ImVec2(from.x, DbToY(0.0)),
								Theme::WithAlpha(th.graphCurveCool, 40));
		drawList->AddLine(from, to, th.graphCurveCool, 2.0f);
	}

	drawList->PopClipRect();

	// ---- interaction ----
	// NOTE: the handles are submitted BEFORE the graph background, and the order is
	// load-bearing. ImGui gives hover to the first item that claims it, so a background
	// button submitted first sets HoveredId and every handle after it fails
	// ItemHoverable - which left all eight of them undraggable. handles first also
	// means a right-click over one opens the band's own menu, not the graph's
	for (int band = 0; band < kNumBands; ++band) {
		BandParams& params = mBands[mEditSet][band];
		const int type = std::clamp((int)std::lround(params.pType->value), 0, (int)EQFilterType::Count - 1);
		const bool active = params.pActive->value > 0.5f;
		const double frequency = (double)params.pFrequency->value;
		const bool usesGain = TypeUsesGain((EQFilterType)type);

		// where the handle rides: on its own gain for the shapes that have one, on the
		// curve for a cut (so a 12 dB/oct corner sits at its -3 dB point) and on the
		// zero line for a notch, whose own response there is a hole
		double handleDb = 0.0;
		if (usesGain) {
			handleDb = (double)params.pGain->value * (double)pScale->value / 100.0;
		} else if (type != (int)EQFilterType::Notch) {
			float own = 0.0f;
			GetResponseDb(mEditSet, band, &frequency, &own, 1);
			handleDb = own;
		}

		const ImVec2 center(FrequencyToX(frequency), DbToY(handleDb));
		const float radius = 8.0f;

		ImGui::PushID(band);
		ImGui::SetCursorScreenPos(ImVec2(center.x - radius, center.y - radius));
		ImGui::InvisibleButton("##Handle", ImVec2(radius * 2.0f, radius * 2.0f));
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();

		// the wheel belongs to the handle while the cursor is on it, or the scroll
		// wrapper the device rack puts around a tall UI would eat the Q edit
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);

		if (ImGui::IsItemActivated()) {
			mSelectedBand = band;
			mDraggingBand = band;
			mDragOldFrequency = params.pFrequency->value;
			mDragOldGain = params.pGain->value;
			mDragOldQ = params.pQ->value;
			mDragFine = ImGui::GetIO().KeyShift;
			mDragAdjustingQ = ImGui::GetIO().KeyAlt;
			AnchorHandleDrag(params);
			params.pFrequency->Select();
		}

		if (held) {
			const ImGuiIO& io = ImGui::GetIO();

			// a modifier pressed or released mid-drag re-pins the anchor, so the handle
			// carries on from where it is instead of jumping to whatever the new scale
			// implies about a movement that happened under the old one
			if (io.KeyShift != mDragFine || io.KeyAlt != mDragAdjustingQ) {
				mDragFine = io.KeyShift;
				mDragAdjustingQ = io.KeyAlt;
				AnchorHandleDrag(params);
			}

			// the axes scale the movement, so an unmodified drag keeps the handle
			// exactly under the cursor - and Shift can still slow it down without the
			// handle tearing away to wherever the pointer went
			const float scale = mDragFine ? 0.2f : 1.0f;
			const float movedX = (io.MousePos.x - mDragAnchorMouse.x) * scale;
			const float movedY = (io.MousePos.y - mDragAnchorMouse.y) * scale;

			if (mDragAdjustingQ) {
				// Alt-drag is Q, the way it is in the original: the band narrows and
				// widens under the cursor and its frequency stays where it was grabbed
				//
				// half the graph height is one decade, so the whole 0.1 to 18 range is
				// a drag and a bit rather than a twitch
				params.pQ->value = std::clamp(mDragAnchorQ * std::pow(10.0f, -movedY / (size.y * 0.5f)),
											  params.pQ->minValue, params.pQ->maxValue);
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
			} else {
				const float ratio = std::pow((float)(kMaxFrequency / kMinFrequency), movedX / size.x);
				params.pFrequency->value = std::clamp(mDragAnchorFrequency * ratio,
													  params.pFrequency->minValue, params.pFrequency->maxValue);
				if (usesGain) {
					params.pGain->value = std::clamp(mDragAnchorGain - movedY * mGraphRangeDb / (size.y * 0.5f),
													 params.pGain->minValue, params.pGain->maxValue);
				}
			}
		}

		if (ImGui::IsItemDeactivated()) {
			mDraggingBand = -1;
			// a drag moves up to two parameters at once and the edit gesture is a single
			// global slot, so each axis that actually moved is committed on release
			// instead. a purely horizontal drag therefore stays one undo entry
			params.pFrequency->CommitEditImmediate(mDragOldFrequency);
			params.pGain->CommitEditImmediate(mDragOldGain);
			params.pQ->CommitEditImmediate(mDragOldQ);
		}

		if (hovered) {
			const float wheel = ImGui::GetIO().MouseWheel;
			if (wheel != 0.0f) {
				const float oldQ = params.pQ->value;
				const float step = ImGui::GetIO().KeyShift ? 1.02f : 1.15f;
				params.pQ->value = std::clamp(oldQ * std::pow(step, wheel), params.pQ->minValue, params.pQ->maxValue);
				params.pQ->CommitEditImmediate(oldQ);
			}

			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				const float oldActive = params.pActive->value;
				params.pActive->value = oldActive > 0.5f ? 0.0f : 1.0f;
				params.pActive->CommitEditImmediate(oldActive);
			}

			if (!held) {
				char tooltip[224];
				snprintf(tooltip, sizeof(tooltip),
						 "%d  %s\n%.0f Hz   %+.1f dB   Q %.2f\n"
						 "Drag: frequency and gain   Alt: Q   Shift: fine\n"
						 "Wheel: Q   Double-click: on/off",
						 band + 1, kFilterTypeNames[type], (double)params.pFrequency->value,
						 (double)params.pGain->value, (double)params.pQ->value);
				ImGui::SetTooltip("%s", tooltip);
			}
		}

		// right-click a handle for its shape, which saves the trip to the strip below
		if (ImGui::BeginPopupContextItem("EQBandMenu")) {
			ImGui::TextDisabled("Band %d", band + 1);
			ImGui::Separator();
			for (int candidate = 0; candidate < (int)EQFilterType::Count; ++candidate) {
				if (ImGui::MenuItem(kFilterTypeNames[candidate], nullptr, candidate == type)) {
					const float oldType = params.pType->value;
					params.pType->value = (float)candidate;
					params.pType->CommitEditImmediate(oldType);
				}
			}
			ImGui::Separator();
			bool bandActive = active;
			if (ImGui::MenuItem("Active", nullptr, &bandActive)) {
				const float oldActive = params.pActive->value;
				params.pActive->value = bandActive ? 1.0f : 0.0f;
				params.pActive->CommitEditImmediate(oldActive);
			}
			ImGui::EndPopup();
		}

		const bool lit = active && (hovered || held || band == mSelectedBand);
		const ImU32 fill = active ? (lit ? th.accentHover : th.accent) : th.bgPanelAlt;
		drawList->AddCircleFilled(center, radius, fill);
		drawList->AddCircle(center, radius, active ? th.bgDeepest : th.border, 0, 1.5f);

		char number[4];
		snprintf(number, sizeof(number), "%d", band + 1);
		const ImVec2 numberSize = ImGui::CalcTextSize(number);
		drawList->AddText(ImVec2(center.x - numberSize.x * 0.5f, center.y - numberSize.y * 0.5f),
						  active ? th.textOnAccent : th.textDim, number);
		ImGui::PopID();
	}

	// the background takes what the handles left, which is every pixel that is not one
	// of them
	ImGui::SetCursorScreenPos(pos);
	ImGui::InvisibleButton("##EQGraph", size, ImGuiButtonFlags_MouseButtonRight);

	if (ImGui::BeginPopupContextItem("EQGraphMenu")) {
		if (ImGui::BeginMenu("Display Range")) {
			for (float range : {6.0f, 15.0f, 30.0f}) {
				char label[16];
				snprintf(label, sizeof(label), "%g dB", range);
				if (ImGui::MenuItem(label, nullptr, mGraphRangeDb == range))
					mGraphRangeDb = range;
			}
			ImGui::EndMenu();
		}
		ImGui::MenuItem("Analyzer", nullptr, &mShowAnalyzer);

		bool oversample = IsOversampling();
		if (ImGui::MenuItem("Oversampling (2x)", nullptr, &oversample))
			SetOversampling(oversample);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Runs the filters at twice the rate so a corner near Nyquist keeps its\n"
							  "shape. Costs CPU and 15 samples of latency that nothing compensates");
		}
		ImGui::EndPopup();
	}

	drawList->AddRect(pos, end, th.border);
	ImGui::SetCursorScreenPos(restore);
}

void EQEightProcessor::DrawBandStrip(float width) {
	const Theme& th = Theme::Instance();
	ImGuiStyle& style = ImGui::GetStyle();
	ImDrawList* drawList = ImGui::GetWindowDrawList();

	const float spacing = 3.0f;
	const float slotWidth = std::max((width - spacing * (float)(kNumBands - 1)) / (float)kNumBands, 26.0f);
	const float rowHeight = ImGui::GetFrameHeight();
	const ImVec2 origin = ImGui::GetCursorScreenPos();

	for (int band = 0; band < kNumBands; ++band) {
		ImGui::PushID(band);
		BandParams& params = mBands[mEditSet][band];
		const int type = std::clamp((int)std::lround(params.pType->value), 0, (int)EQFilterType::Count - 1);
		const bool active = params.pActive->value > 0.5f;
		const ImVec2 slotPos(origin.x + (float)band * (slotWidth + spacing), origin.y);

		// ---- type button ----
		ImGui::SetCursorScreenPos(slotPos);
		if (ImGui::InvisibleButton("##Type", ImVec2(slotWidth, rowHeight)))
			ImGui::OpenPopup("EQTypeMenu");

		const bool typeHovered = ImGui::IsItemHovered();
		const ImVec2 typeEnd(slotPos.x + slotWidth, slotPos.y + rowHeight);
		drawList->AddRectFilled(slotPos, typeEnd, typeHovered ? th.bgHover : th.bgPanelAlt, style.FrameRounding);
		drawList->AddRect(slotPos, typeEnd, band == mSelectedBand ? th.accent : th.border, style.FrameRounding);
		DrawFilterIcon(drawList, (EQFilterType)type,
					   ImVec2(slotPos.x + 3.0f, slotPos.y + 2.0f),
					   ImVec2(slotWidth - 14.0f, rowHeight - 4.0f),
					   active ? th.text : th.textDim);
		// the dropdown marker, drawn small enough to survive a narrow slot
		drawList->AddTriangleFilled(ImVec2(typeEnd.x - 9.0f, slotPos.y + rowHeight * 0.5f - 2.0f),
									ImVec2(typeEnd.x - 3.0f, slotPos.y + rowHeight * 0.5f - 2.0f),
									ImVec2(typeEnd.x - 6.0f, slotPos.y + rowHeight * 0.5f + 2.0f),
									th.textMuted);
		if (typeHovered)
			ImGui::SetTooltip("Band %d: %s", band + 1, kFilterTypeNames[type]);

		if (ImGui::BeginPopup("EQTypeMenu")) {
			for (int candidate = 0; candidate < (int)EQFilterType::Count; ++candidate) {
				const ImVec2 iconPos = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(22.0f, ImGui::GetTextLineHeight()));
				DrawFilterIcon(ImGui::GetWindowDrawList(), (EQFilterType)candidate,
							   ImVec2(iconPos.x, iconPos.y), ImVec2(18.0f, ImGui::GetTextLineHeight()), th.text);
				ImGui::SameLine();
				if (ImGui::Selectable(kFilterTypeNames[candidate], candidate == type)) {
					const float oldType = params.pType->value;
					params.pType->value = (float)candidate;
					params.pType->CommitEditImmediate(oldType);
					mSelectedBand = band;
				}
			}
			ImGui::EndPopup();
		}

		// ---- enable toggle and band number ----
		const ImVec2 togglePos(slotPos.x, slotPos.y + rowHeight + 2.0f);
		const float boxSize = ImGui::GetTextLineHeight();
		ImGui::SetCursorScreenPos(togglePos);
		if (ImGui::InvisibleButton("##Active", ImVec2(boxSize, boxSize))) {
			const float oldActive = params.pActive->value;
			params.pActive->value = active ? 0.0f : 1.0f;
			params.pActive->CommitEditImmediate(oldActive);
		}
		if (ImGui::IsItemHovered()) {
			if (active)
				ImGui::SetTooltip("Band %d is on", band + 1);
			else
				ImGui::SetTooltip("Band %d is off", band + 1);
		}

		const ImVec2 boxEnd(togglePos.x + boxSize, togglePos.y + boxSize);
		drawList->AddRectFilled(togglePos, boxEnd, active ? th.graphCurveCool : th.inputBg, 2.0f);
		drawList->AddRect(togglePos, boxEnd, th.border, 2.0f);

		ImGui::SetCursorScreenPos(ImVec2(boxEnd.x + 3.0f, togglePos.y));
		char number[4];
		snprintf(number, sizeof(number), "%d", band + 1);
		if (ImGui::InvisibleButton("##Select", ImVec2(std::max(slotWidth - boxSize - 3.0f, 8.0f), boxSize)))
			mSelectedBand = band;
		drawList->AddText(ImVec2(boxEnd.x + 4.0f, togglePos.y), band == mSelectedBand ? th.accent : th.textMuted, number);

		ImGui::PopID();
	}

	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rowHeight + 2.0f + ImGui::GetTextLineHeight()));
}

void EQEightProcessor::DrawGlobals(float width) {
	const Theme& th = Theme::Instance();
	ImGuiStyle& style = ImGui::GetStyle();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const EQChannelMode mode = GetChannelMode();

	// tighter than the default row spacing, because six controls have to fit beside a
	// graph that is already the shortest it can usefully be. RenderCustomUI measures
	// the column with this same number
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, (float)kGlobalsRowSpacing));

	// ---- audition and analyzer ----
	const float iconSize = ImGui::GetFrameHeight();
	const bool audition = mAudition.load(std::memory_order_relaxed);
	ImVec2 iconPos = ImGui::GetCursorScreenPos();

	if (ImGui::InvisibleButton("##Audition", ImVec2(iconSize, iconSize)))
		mAudition.store(!audition, std::memory_order_relaxed);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Audition: hear only the band you are editing");
	drawList->AddRectFilled(iconPos, ImVec2(iconPos.x + iconSize, iconPos.y + iconSize),
							audition ? th.accent : th.bgPanelAlt, style.FrameRounding);
	{
		// a headphone: the band over the top, a cup on each side
		const ImU32 glyph = audition ? th.textOnAccent : th.textMuted;
		const ImVec2 center(iconPos.x + iconSize * 0.5f, iconPos.y + iconSize * 0.58f);
		drawList->PathArcTo(center, iconSize * 0.26f, (float)kPi, 2.0f * (float)kPi, 12);
		drawList->PathStroke(glyph, 0, 1.5f);
		drawList->AddRectFilled(ImVec2(center.x - iconSize * 0.32f, center.y - iconSize * 0.04f),
								ImVec2(center.x - iconSize * 0.18f, center.y + iconSize * 0.20f), glyph, 1.5f);
		drawList->AddRectFilled(ImVec2(center.x + iconSize * 0.18f, center.y - iconSize * 0.04f),
								ImVec2(center.x + iconSize * 0.32f, center.y + iconSize * 0.20f), glyph, 1.5f);
	}

	ImGui::SameLine(0.0f, 3.0f);
	iconPos = ImGui::GetCursorScreenPos();
	if (ImGui::InvisibleButton("##Analyzer", ImVec2(iconSize, iconSize)))
		mShowAnalyzer = !mShowAnalyzer;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Show the spectrum behind the curve");
	drawList->AddRectFilled(iconPos, ImVec2(iconPos.x + iconSize, iconPos.y + iconSize),
							mShowAnalyzer ? th.accent : th.bgPanelAlt, style.FrameRounding);
	{
		// four bars of a spectrum, tallest in the middle
		const ImU32 glyph = mShowAnalyzer ? th.textOnAccent : th.textMuted;
		const float heights[4] = {0.35f, 0.60f, 0.45f, 0.25f};
		for (int bar = 0; bar < 4; ++bar) {
			const float x = iconPos.x + iconSize * (0.24f + 0.14f * (float)bar);
			const float bottom = iconPos.y + iconSize * 0.74f;
			drawList->AddRectFilled(ImVec2(x, bottom - iconSize * heights[bar]), ImVec2(x + iconSize * 0.08f, bottom), glyph);
		}
	}

	// ---- mode ----
	ImGui::TextUnformatted("Mode");
	ImGui::SetNextItemWidth(width);
	if (ImGui::BeginCombo("##EQMode", kChannelModeNames[(int)mode], ImGuiComboFlags_NoArrowButton)) {
		for (int candidate = 0; candidate < (int)EQChannelMode::Count; ++candidate) {
			if (ImGui::Selectable(kChannelModeNames[candidate], candidate == (int)mode)) {
				const float oldMode = pMode->value;
				pMode->value = (float)candidate;
				pMode->CommitEditImmediate(oldMode);
				if (candidate == (int)EQChannelMode::Stereo)
					mEditSet = 0;
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Stereo: one curve on both channels\n"
						  "L/R: a separate curve per channel\n"
						  "M/S: a separate curve for the middle and the sides");
	}

	// ---- edit ----
	ImGui::TextUnformatted("Edit");
	const float halfWidth = (width - 3.0f) * 0.5f;
	for (int set = 0; set < kNumSets; ++set) {
		if (set > 0)
			ImGui::SameLine(0.0f, 3.0f);
		ImGui::PushID(set);
		const bool selected = set == mEditSet;
		const bool enabled = set == 0 || mode != EQChannelMode::Stereo;
		ImGui::PushStyleColor(ImGuiCol_Button, selected && enabled ? th.accent : th.bgPanelAlt);
		ImGui::PushStyleColor(ImGuiCol_Text, selected && enabled ? th.textOnAccent : (enabled ? th.text : th.textDim));
		if (ImGui::Button(kSetNames[(int)mode][set], ImVec2(halfWidth, 0.0f)) && enabled)
			mEditSet = set;
		ImGui::PopStyleColor(2);
		ImGui::PopID();
	}
	if (mode == EQChannelMode::Stereo && ImGui::IsItemHovered())
		ImGui::SetTooltip("Only L/R and M/S have a second curve to edit");

	// ---- adaptive q ----
	ImGui::TextUnformatted("Adapt. Q");
	const bool adaptive = pAdaptQ->value > 0.5f;
	ImGui::PushStyleColor(ImGuiCol_Button, adaptive ? th.accent : th.bgPanelAlt);
	ImGui::PushStyleColor(ImGuiCol_Text, adaptive ? th.textOnAccent : th.text);
	if (ImGui::Button(adaptive ? "On" : "Off", ImVec2(width, 0.0f))) {
		const float oldValue = pAdaptQ->value;
		pAdaptQ->value = adaptive ? 0.0f : 1.0f;
		pAdaptQ->CommitEditImmediate(oldValue);
	}
	ImGui::PopStyleColor(2);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Narrows a bell as it is boosted or cut, the way an analog EQ does");

	// ---- scale and output gain ----
	ImGui::TextUnformatted("Scale");
	pScale->DrawCompact(width, "%.0f %%", true);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Rides every shelf and bell gain at once");

	ImGui::TextUnformatted("Gain");
	pOutputGain->DrawCompact(width, "%+.2f dB", false);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Output level, after the whole chain");

	ImGui::PopStyleVar();
}

bool EQEightProcessor::RenderCustomUI(const ImVec2& size) {
	ImGuiStyle& style = ImGui::GetStyle();

	// too cramped to lay out honestly - let the rack fall back to the parameter list
	if (size.x < 300.0f || size.y < 150.0f)
		return false;

	const EQChannelMode mode = GetChannelMode();
	if (mode == EQChannelMode::Stereo)
		mEditSet = 0; // there is no B to edit
	mSelectedBand = std::clamp(mSelectedBand, 0, kNumBands - 1);
	mEditSet = std::clamp(mEditSet, 0, kNumSets - 1);

	// the audio thread auditions whichever band the editor is pointing at
	mAuditionBand.store(mSelectedBand, std::memory_order_relaxed);
	mAuditionSet.store(mEditSet, std::memory_order_relaxed);

	// a knob is label + dial + value, and three of them stacked set the floor for how
	// short the graph beside them is allowed to be
	const float knobHeight = ImGui::GetTextLineHeight() * 2.0f + style.ItemInnerSpacing.y * 2.0f + 36.0f;
	const float knobColumnHeight = knobHeight * 3.0f + style.ItemSpacing.y * 2.0f;
	const float knobWidth = ImGui::CalcTextSize("-15.0 dB").x + 8.0f;
	const float globalsWidth = std::max(ImGui::CalcTextSize("Stereo").x + style.FramePadding.x * 4.0f, 64.0f);
	const float stripHeight = ImGui::GetFrameHeight() + ImGui::GetTextLineHeight() + 2.0f;

	// the globals column is taller still: an icon row over five label-plus-control
	// pairs. whichever column is tallest decides where the band strip starts
	const float globalsColumnHeight = ImGui::GetFrameHeight() * 6.0f + ImGui::GetTextLineHeight() * 5.0f +
									  (float)kGlobalsRowSpacing * 10.0f;

	const float graphWidth = std::max(size.x - knobWidth - globalsWidth - style.ItemSpacing.x * 2.0f, 160.0f);
	const float columnHeight = std::max(knobColumnHeight, globalsColumnHeight);
	const float graphHeight = std::max(size.y - stripHeight - style.ItemSpacing.y, columnHeight);
	const float contentHeight = graphHeight + style.ItemSpacing.y + stripHeight;

	// every column is placed explicitly rather than with SameLine: the graph submits
	// its own items at absolute positions, which leaves nothing sane for SameLine to
	// measure against. the whole block is reserved up front so the parent knows how
	// far it runs, and closed with an item at the end - ImGui asserts on a child that
	// ends on a bare SetCursorScreenPos
	const ImVec2 origin = ImGui::GetCursorScreenPos();

	BandParams& selected = mBands[mEditSet][mSelectedBand];
	ImGui::SetCursorScreenPos(origin);
	ImGui::BeginGroup();
	selected.pFrequency->Draw();
	selected.pGain->Draw();
	selected.pQ->Draw();
	ImGui::EndGroup();

	DrawGraph(ImVec2(origin.x + knobWidth + style.ItemSpacing.x, origin.y), ImVec2(graphWidth, graphHeight));

	ImGui::SetCursorScreenPos(ImVec2(origin.x + knobWidth + graphWidth + style.ItemSpacing.x * 2.0f, origin.y));
	ImGui::BeginGroup();
	DrawGlobals(globalsWidth);
	ImGui::EndGroup();

	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + graphHeight + style.ItemSpacing.y));
	DrawBandStrip(size.x);

	// leave the cursor under the block, on an item rather than a bare cursor move
	ImGui::SetCursorScreenPos(origin);
	ImGui::Dummy(ImVec2(size.x, contentHeight));

	return true;
}
