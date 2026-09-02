#include "PrecompHeader.h"
#include "AnalyzerProcessor.h"
#include "Analysis/Spectrum.h"
#include "ProcessorFactory.h"
#include "Theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

REGISTER_PROCESSOR(AnalyzerProcessor, "Analyzer", false)

namespace {

	constexpr double kPi = 3.14159265358979323846;

	// the frequency axis, shared with EQ Eight so the two devices' spectra line up
	// when they sit next to each other in a rack
	constexpr double kMinFrequency = 10.0;
	constexpr double kMaxFrequency = 22000.0;

	// tilt pivots here, so turning it up rotates the curve about the middle of the
	// graph instead of lifting the whole thing
	constexpr double kTiltPivotFrequency = 1000.0;

	// BS.1770-4's absolute gate, and the -10 LU relative gate that follows it
	constexpr double kAbsoluteGateLoudness = -70.0;
	constexpr double kIntegratedRelativeGate = 10.0;

	// EBU Tech 3342 gates the range 20 LU down instead, and reports the spread
	// between these two percentiles of what survives
	constexpr double kRangeRelativeGate = 20.0;
	constexpr double kRangeLowPercentile = 0.10;
	constexpr double kRangeHighPercentile = 0.95;

	// inter-sample peaks are a question about signals near full scale. a block whose
	// sample peak is this far down cannot hide one worth reporting, so it skips the
	// 48-tap interpolation entirely and the meter costs nothing on a quiet track
	constexpr float kTruePeakGate = 0.25f; // -12 dBFS

	// how fast a display curve falls back. instant attack, so a transient shows up at
	// its real height, then a slow release rather than a flicker
	constexpr float kSpectrumRelease = 0.30f;
	constexpr float kPeakHoldFallDb = 0.20f;

	// the spectrogram scrolls on its own clock so the picture means the same thing at
	// 30 and at 144 frames a second
	constexpr float kSpectrogramInterval = 1.0f / 40.0f;

	const char* kViewNames[(int)AnalyzerView::Count] = {
		"Spectrum", "Sonogram", "Stereo", "Loudness", "Scope"};

	const char* kChannelModeNames[(int)AnalyzerChannelMode::Count] = {"L / R", "M / S", "Mono"};

	const char* kTriggerNames[(int)AnalyzerTriggerMode::Count] = {"Free", "Edge", "Beat"};

	const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

	double LinearToDb(double linear) {
		return 20.0 * std::log10(std::max(linear, 1.0e-9));
	}

	// BS.1770-4's loudness from a K-weighted mean square. the offset is the standard's
	// own calibration constant, not a fudge
	double PowerToLoudness(double power) {
		if (power <= 1.0e-12)
			return -100.0;
		return std::max(-0.691 + 10.0 * std::log10(power), -100.0);
	}

	// the nearest note to a frequency, plus how many cents off it is - which is what
	// turns "there is a resonance at 155 Hz" into "there is a resonance on your D#3"
	void FrequencyToNote(double frequency, char* out, size_t size) {
		if (frequency < 8.0) {
			snprintf(out, size, "--");
			return;
		}
		const double midi = 69.0 + 12.0 * std::log2(frequency / 440.0);
		const int rounded = (int)std::lround(midi);
		const int note = ((rounded % 12) + 12) % 12;
		const int octave = rounded / 12 - 1;
		const int cents = (int)std::lround((midi - (double)rounded) * 100.0);
		snprintf(out, size, "%s%d %+d", kNoteNames[note], octave, cents);
	}

	void FormatFrequency(double frequency, char* out, size_t size) {
		if (frequency >= 1000.0)
			snprintf(out, size, "%.2f kHz", frequency / 1000.0);
		else
			snprintf(out, size, "%.0f Hz", frequency);
	}

	// the vertical rules behind both frequency views. the flag picks which ones carry
	// a label, so the decade marks stay readable while the rest stay quiet
	struct GridLine {
		double frequency;
		const char* label;
	};

	const GridLine kGridLines[] = {
		{20.0, "20"}, {30.0, nullptr}, {40.0, nullptr}, {50.0, "50"}, {60.0, nullptr},
		{80.0, nullptr}, {100.0, "100"}, {200.0, "200"}, {300.0, nullptr}, {400.0, nullptr},
		{500.0, "500"}, {600.0, nullptr}, {800.0, nullptr}, {1000.0, "1k"}, {2000.0, "2k"},
		{3000.0, nullptr}, {4000.0, nullptr}, {5000.0, "5k"}, {6000.0, nullptr},
		{8000.0, nullptr}, {10000.0, "10k"}, {15000.0, nullptr}, {20000.0, "20k"}};

	// the 4x polyphase interpolator behind the true peak meter, laid out phase-major so
	// one output tap is a contiguous run. a windowed sinc designed on first use rather
	// than the table in the annex, which only exists for one sample rate anyway
	const double* TruePeakKernel() {
		static const std::vector<double> kernel = [] {
			constexpr int phases = 4;
			constexpr int taps = 12;
			constexpr int total = phases * taps;

			std::vector<double> prototype((size_t)total, 0.0);
			const double center = (double)(total - 1) * 0.5;
			for (int n = 0; n < total; ++n) {
				const double x = ((double)n - center) / (double)phases;
				const double sinc = std::fabs(x) < 1.0e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
				const double phase = 2.0 * kPi * (double)n / (double)(total - 1);
				const double window = 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
				prototype[(size_t)n] = sinc * window;
			}

			// polyphase split: output i + p/4 is sum over t of h[p + 4t] * x[i - t]. each
			// phase is normalized to unity dc gain on its own, or a held sample would read
			// four different levels depending on which phase happened to look at it
			std::vector<double> split((size_t)total, 0.0);
			for (int p = 0; p < phases; ++p) {
				double sum = 0.0;
				for (int t = 0; t < taps; ++t)
					sum += prototype[(size_t)(p + t * phases)];
				const double scale = std::fabs(sum) > 1.0e-12 ? 1.0 / sum : 1.0;
				for (int t = 0; t < taps; ++t)
					split[(size_t)(p * taps + t)] = prototype[(size_t)(p + t * phases)] * scale;
			}
			return split;
		}();
		return kernel.data();
	}

} // namespace

// ================================================================
// LIFECYCLE
// ================================================================

AnalyzerProcessor::AnalyzerProcessor() {
	// NOTE: no parameters. everything here is a switch on a readout, and an analyzer
	// with an automation lane would only invite someone to automate their own meter
	for (int trace = 0; trace < 2; ++trace) {
		mSpectrumDb[trace].assign(kSpectrumPoints, -140.0f);
		mSpectrumPeakDb[trace].assign(kSpectrumPoints, -140.0f);
		mScratchSamples[trace].assign(kTransformSize, 0.0f);
		mScratchReal[trace].assign(kTransformSize, 0.0);
		mScratchImaginary[trace].assign(kTransformSize, 0.0);
	}
	mScratchMagnitude.assign(kTransformSize / 2 + 1, 0.0);
	mScratchDb.assign(kSpectrumPoints, -140.0f);
	mSpectrogram.assign((size_t)kSpectrogramColumns * kSpectrogramRows, -140.0f);

	mRingLeft.assign(kRingSize, 0.0f);
	mRingRight.assign(kRingSize, 0.0f);

	PrepareToPlay(48000.0);
}

void AnalyzerProcessor::PrepareToPlay(double sampleRate) {
	mSampleRate = sampleRate > 1.0 ? sampleRate : 48000.0;

	// one-pole coefficients for the ~300 ms level averages and the ~1 s dc mean
	mMeterCoefficient = 1.0 - std::exp(-1.0 / (0.300 * mSampleRate));
	mOffsetCoefficient = 1.0 - std::exp(-1.0 / (1.000 * mSampleRate));

	mSubBlockSamples = std::max((int)std::lround(mSampleRate * 0.100), 1);

	DesignWeighting(mSampleRate);
	ClearAudioState();
	ResetMeasurements();
}

void AnalyzerProcessor::Reset() {
	// a transport stop should not throw away an integrated loudness that was measured
	// over the take that just played, so this clears the live capture only. the held
	// peaks and the loudness history go through ResetMeasurements, which is a button
	std::fill(mRingLeft.begin(), mRingLeft.end(), 0.0f);
	std::fill(mRingRight.begin(), mRingRight.end(), 0.0f);
	ClearAudioState();
}

void AnalyzerProcessor::ClearAudioState() {
	mRMSState = {};
	mOffsetState = {};
	mHeldPeakState = {};
	mCorrelationLeftRight = 0.0;
	mCorrelationLeftLeft = 0.0;
	mCorrelationRightRight = 0.0;
	mWeightState = {};
	mTruePeakTail = {};
	mSubBlockFill = 0;
	mSubBlockSum = 0.0;
	mSubBlockPower = {};
	mSubBlockWrite = 0;
	mSubBlockCount = 0;

	for (int channel = 0; channel < 2; ++channel) {
		mPeak[channel].store(0.0f, std::memory_order_relaxed);
		mHeldPeak[channel].store(0.0f, std::memory_order_relaxed);
		mTruePeak[channel].store(0.0f, std::memory_order_relaxed);
		mRMS[channel].store(0.0f, std::memory_order_relaxed);
		mOffset[channel].store(0.0f, std::memory_order_relaxed);
	}
	mCorrelation.store(1.0f, std::memory_order_relaxed);
	mMomentaryLoudness.store(-100.0f, std::memory_order_relaxed);
	mShortTermLoudness.store(-100.0f, std::memory_order_relaxed);
}

void AnalyzerProcessor::ResetMeasurements() {
	mResetRequest.store(true, std::memory_order_relaxed);

	mIntegratedPower.clear();
	mRangePower.clear();
	mIntegratedLoudness = -100.0f;
	mLoudnessRange = 0.0f;

	// drop whatever the audio thread has already queued: it belongs to the take that
	// was just discarded
	mLoudnessRead = mLoudnessWrite.load(std::memory_order_acquire);

	std::fill(mSpectrogram.begin(), mSpectrogram.end(), -140.0f);
	for (int trace = 0; trace < 2; ++trace)
		std::fill(mSpectrumPeakDb[trace].begin(), mSpectrumPeakDb[trace].end(), -140.0f);
}

// ================================================================
// AUDIO THREAD
// ================================================================

void AnalyzerProcessor::DesignWeighting(double rate) {
	// BS.1770-4's two stages: a high shelf standing in for the head, then the RLB
	// high pass. the standard tabulates coefficients at 48 kHz only, so both are
	// designed analytically here - at 48 kHz this reproduces that table to six places
	{
		const double frequency = 1681.9744509555319;
		const double gainDb = 3.999843853973347;
		const double q = 0.7071752369554196;
		const double k = std::tan(kPi * frequency / rate);
		const double shelf = std::pow(10.0, gainDb / 20.0);
		const double bandwidth = std::pow(shelf, 0.4996667741545416);
		const double denominator = 1.0 + k / q + k * k;
		mWeightShelf.b0 = (shelf + bandwidth * k / q + k * k) / denominator;
		mWeightShelf.b1 = 2.0 * (k * k - shelf) / denominator;
		mWeightShelf.b2 = (shelf - bandwidth * k / q + k * k) / denominator;
		mWeightShelf.a1 = 2.0 * (k * k - 1.0) / denominator;
		mWeightShelf.a2 = (1.0 - k / q + k * k) / denominator;
	}
	{
		const double frequency = 38.13547087602444;
		const double q = 0.5003270373238773;
		const double k = std::tan(kPi * frequency / rate);
		const double denominator = 1.0 + k / q + k * k;

		// NOTE: the numerator is 1, -2, 1 unnormalized, exactly as the standard prints
		// it. dividing it through would be the textbook design and would sit 0.04 dB
		// below every published reference measurement
		mWeightHighPass.b0 = 1.0;
		mWeightHighPass.b1 = -2.0;
		mWeightHighPass.b2 = 1.0;
		mWeightHighPass.a1 = 2.0 * (k * k - 1.0) / denominator;
		mWeightHighPass.a2 = (1.0 - k / q + k * k) / denominator;
	}
}

double AnalyzerProcessor::ApplyBiquad(const AnalyzerBiquad& coefficients, AnalyzerBiquadState& state, double x) {
	const double y = coefficients.b0 * x + coefficients.b1 * state.x1 + coefficients.b2 * state.x2
					 - coefficients.a1 * state.y1 - coefficients.a2 * state.y2;
	state.x2 = state.x1;
	state.x1 = x;
	state.y2 = state.y1;
	state.y1 = y;
	return y;
}

double AnalyzerProcessor::MeanSubBlockPower(int count) const {
	const int available = std::min(count, mSubBlockCount);
	if (available <= 0)
		return 0.0;

	double sum = 0.0;
	for (int i = 0; i < available; ++i) {
		const int index = (mSubBlockWrite - 1 - i + kSubBlocksPerShortTerm * 2) % kSubBlocksPerShortTerm;
		sum += mSubBlockPower[(size_t)index];
	}
	return sum / (double)available;
}

void AnalyzerProcessor::CloseSubBlock() {
	// z is the mean square per channel summed over channels, which is what the
	// standard's G-weighted sum reduces to for a plain stereo pair
	const double power = mSubBlockSum / (double)mSubBlockFill;
	mSubBlockPower[(size_t)mSubBlockWrite] = power;
	mSubBlockWrite = (mSubBlockWrite + 1) % kSubBlocksPerShortTerm;
	if (mSubBlockCount < kSubBlocksPerShortTerm)
		++mSubBlockCount;
	mSubBlockFill = 0;
	mSubBlockSum = 0.0;

	// every sub-block is the same length, so a mean of sub-block means is the mean
	const double momentaryPower = MeanSubBlockPower(kSubBlocksPerMomentary);
	const double shortTermPower = MeanSubBlockPower(kSubBlocksPerShortTerm);
	mMomentaryLoudness.store((float)PowerToLoudness(momentaryPower), std::memory_order_relaxed);
	mShortTermLoudness.store((float)PowerToLoudness(shortTermPower), std::memory_order_relaxed);

	// hand both to the UI thread, which does the gating this thread must not stop for.
	// the index is monotonic and masked on access, so the reader can tell a stall that
	// overran the ring from an ordinary lap
	const int write = mLoudnessWrite.load(std::memory_order_relaxed);
	LoudnessBlock& block = mLoudnessRing[(size_t)(write & (kLoudnessRingSize - 1))];
	block.momentaryPower = (float)momentaryPower;
	block.shortTermPower = (float)shortTermPower;
	block.shortTermValid = mSubBlockCount >= kSubBlocksPerShortTerm;
	mLoudnessWrite.store(write + 1, std::memory_order_release);
}

float AnalyzerProcessor::MeasureTruePeak(const float* buffer, int numFrames, int numChannels,
										 int bufferChannel, int slot, bool measure) {
	// the previous block's tail is copied out first: when a block is shorter than the
	// kernel, carrying the new tail would otherwise overwrite samples still being read
	const std::array<float, kTruePeakTaps> previous = mTruePeakTail[(size_t)slot];

	auto SampleAt = [&](int index) -> double {
		if (index >= 0)
			return (double)buffer[(size_t)index * numChannels + bufferChannel];
		const int back = kTruePeakTaps + index;
		return back >= 0 ? (double)previous[(size_t)back] : 0.0;
	};

	float peak = 0.0f;
	if (measure) {
		const double* kernel = TruePeakKernel();
		for (int i = 0; i < numFrames; ++i) {
			for (int phase = 0; phase < kTruePeakPhases; ++phase) {
				double sum = 0.0;
				for (int tap = 0; tap < kTruePeakTaps; ++tap)
					sum += kernel[phase * kTruePeakTaps + tap] * SampleAt(i - tap);
				peak = std::max(peak, (float)std::fabs(sum));
			}
		}
	}

	std::array<float, kTruePeakTaps>& tail = mTruePeakTail[(size_t)slot];
	for (int tap = 0; tap < kTruePeakTaps; ++tap)
		tail[(size_t)tap] = (float)SampleAt(numFrames - kTruePeakTaps + tap);

	return peak;
}

void AnalyzerProcessor::Process(float* buffer, int numFrames, int numChannels,
								std::vector<MIDIMessage>& mIDIMessages,
								const ProcessContext& context) {
	(void)mIDIMessages;

	// NOTE: nothing below writes to `buffer`, and nothing ever should. a meter that
	// changes what it measures is worse than no meter
	if (!buffer || numFrames <= 0 || numChannels <= 0)
		return;

	if (mResetRequest.exchange(false, std::memory_order_relaxed))
		ClearAudioState();

	const int rightChannel = numChannels > 1 ? 1 : 0;

	// a mono chain has one channel of loudness to report, not the same one twice
	const double rightWeight = numChannels > 1 ? 1.0 : 0.0;

	float blockPeakLeft = 0.0f;
	float blockPeakRight = 0.0f;
	int write = mRingWrite.load(std::memory_order_relaxed);

	for (int i = 0; i < numFrames; ++i) {
		const float left = buffer[(size_t)i * numChannels];
		const float right = buffer[(size_t)i * numChannels + rightChannel];

		mRingLeft[(size_t)write] = left;
		mRingRight[(size_t)write] = right;
		write = (write + 1) & (kRingSize - 1);

		blockPeakLeft = std::max(blockPeakLeft, std::fabs(left));
		blockPeakRight = std::max(blockPeakRight, std::fabs(right));

		// one-pole averages rather than a boxcar: no history to carry across blocks,
		// and a transient decays out instead of dropping off a cliff one window later
		mRMSState[0] += ((double)left * left - mRMSState[0]) * mMeterCoefficient;
		mRMSState[1] += ((double)right * right - mRMSState[1]) * mMeterCoefficient;
		mOffsetState[0] += ((double)left - mOffsetState[0]) * mOffsetCoefficient;
		mOffsetState[1] += ((double)right - mOffsetState[1]) * mOffsetCoefficient;

		mCorrelationLeftRight += ((double)left * right - mCorrelationLeftRight) * mMeterCoefficient;
		mCorrelationLeftLeft += ((double)left * left - mCorrelationLeftLeft) * mMeterCoefficient;
		mCorrelationRightRight += ((double)right * right - mCorrelationRightRight) * mMeterCoefficient;

		const double weightedLeft = ApplyBiquad(mWeightHighPass, mWeightState[1][0],
												ApplyBiquad(mWeightShelf, mWeightState[0][0], (double)left));
		const double weightedRight = ApplyBiquad(mWeightHighPass, mWeightState[1][1],
												 ApplyBiquad(mWeightShelf, mWeightState[0][1], (double)right));
		mSubBlockSum += weightedLeft * weightedLeft + rightWeight * weightedRight * weightedRight;
		if (++mSubBlockFill >= mSubBlockSamples)
			CloseSubBlock();
	}

	mRingWrite.store(write, std::memory_order_relaxed);
	mRingSample.store(context.currentSample + numFrames, std::memory_order_relaxed);
	mSamplesPerBeat.store(context.sampleRate * 60.0 / std::max(context.bpm, 1.0), std::memory_order_relaxed);
	mPlaying.store(context.isPlaying, std::memory_order_relaxed);

	// ---- publish ----
	const bool measureTruePeak = std::max(blockPeakLeft, blockPeakRight) > kTruePeakGate;
	const float interpolatedLeft = MeasureTruePeak(buffer, numFrames, numChannels, 0, 0, measureTruePeak);
	const float interpolatedRight = MeasureTruePeak(buffer, numFrames, numChannels, rightChannel, 1, measureTruePeak);

	const float peaks[2] = {blockPeakLeft, blockPeakRight};
	const float truePeaks[2] = {std::max(blockPeakLeft, interpolatedLeft),
								std::max(blockPeakRight, interpolatedRight)};
	for (int channel = 0; channel < 2; ++channel) {
		mPeak[channel].store(peaks[channel], std::memory_order_relaxed);
		mRMS[channel].store((float)std::sqrt(std::max(mRMSState[channel], 0.0)), std::memory_order_relaxed);
		mOffset[channel].store((float)mOffsetState[channel], std::memory_order_relaxed);

		mHeldPeakState[channel] = std::max(mHeldPeakState[channel], (double)truePeaks[channel]);
		mHeldPeak[channel].store((float)mHeldPeakState[channel], std::memory_order_relaxed);
		mTruePeak[channel].store(truePeaks[channel], std::memory_order_relaxed);
	}

	// a silent pair has no phase relationship to report; +1 reads as "centered", which
	// is the least alarming thing to say about nothing
	const double correlationDenominator = std::sqrt(mCorrelationLeftLeft * mCorrelationRightRight);
	float correlation = 1.0f;
	if (correlationDenominator > 1.0e-12)
		correlation = (float)std::clamp(mCorrelationLeftRight / correlationDenominator, -1.0, 1.0);
	mCorrelation.store(correlation, std::memory_order_relaxed);
}

// ================================================================
// ANALYSIS (UI THREAD)
// ================================================================

void AnalyzerProcessor::SetView(AnalyzerView view) {
	mView = std::clamp((int)view, 0, (int)AnalyzerView::Count - 1);
}

void AnalyzerProcessor::SetChannelMode(AnalyzerChannelMode mode) {
	mChannelMode = std::clamp((int)mode, 0, (int)AnalyzerChannelMode::Count - 1);
}

void AnalyzerProcessor::SetTilt(float dbPerOctave) {
	mTiltDbPerOctave = std::clamp(dbPerOctave, 0.0f, 6.0f);
}

double AnalyzerProcessor::GetSpectrumPointFrequency(int index) const {
	return spectrum::LogAxisPointFrequency(std::clamp(index, 0, kSpectrumPoints - 1),
										   kSpectrumPoints, kMinFrequency, kMaxFrequency);
}

float AnalyzerProcessor::GetSpectrumPointDb(int trace, int index) const {
	trace = std::clamp(trace, 0, 1);
	index = std::clamp(index, 0, kSpectrumPoints - 1);
	return mSpectrumDb[(size_t)trace][(size_t)index];
}

float AnalyzerProcessor::TiltedDb(float db, double frequency) const {
	if (mTiltDbPerOctave == 0.0f)
		return db;
	return db + mTiltDbPerOctave * (float)std::log2(frequency / kTiltPivotFrequency);
}

void AnalyzerProcessor::Refresh() {
	// the loudness history is drained even while frozen: freezing is about holding a
	// picture still, not about abandoning a measurement that has been running for
	// three minutes
	DrainLoudnessBlocks();
	if (mFrozen)
		return;

	ComputeCrestFactor();

	// two 4096-point transforms a frame is the expensive part of this device, and only
	// three of the five tabs read them. the loudness and scope tabs are fed entirely by
	// the audio thread, so on those this costs nothing at all
	//
	// NOTE: a caller driving the readout headlessly therefore has to select a view that
	// draws a spectrum before the spectrum and stereo band getters mean anything. the
	// default view does
	const AnalyzerView view = (AnalyzerView)mView;
	if (view == AnalyzerView::Spectrum || view == AnalyzerView::Spectrogram || view == AnalyzerView::Stereo) {
		// ComputeSpectra runs the stereo bands itself, off the untouched left/right
		// transforms and before the display traces fold them into mid/side
		ComputeSpectra();
	}
}

void AnalyzerProcessor::ComputeSpectra() {
	const int write = mRingWrite.load(std::memory_order_relaxed);
	for (int i = 0; i < kTransformSize; ++i) {
		const int index = (write - kTransformSize + i) & (kRingSize - 1);
		mScratchSamples[0][(size_t)i] = mRingLeft[(size_t)index];
		mScratchSamples[1][(size_t)i] = mRingRight[(size_t)index];
	}

	for (int channel = 0; channel < 2; ++channel) {
		spectrum::WindowedTransform(mScratchSamples[channel].data(), kTransformSize,
									mScratchReal[channel].data(), mScratchImaginary[channel].data());
	}

	// the stereo bands read the untouched left/right transforms, so they run before the
	// display traces are folded down onto whatever the channel mode asked for
	ComputeStereoBands();

	const int mode = std::clamp(mChannelMode, 0, (int)AnalyzerChannelMode::Count - 1);
	if (mode == (int)AnalyzerChannelMode::MidSide || mode == (int)AnalyzerChannelMode::Mono) {
		// the transform is linear, so the mid/side decomposition can be taken bin by bin
		// instead of running two more transforms over the summed time signals
		for (int i = 0; i <= kTransformSize / 2; ++i) {
			const double midReal = (mScratchReal[0][(size_t)i] + mScratchReal[1][(size_t)i]) * 0.5;
			const double midImaginary = (mScratchImaginary[0][(size_t)i] + mScratchImaginary[1][(size_t)i]) * 0.5;
			const double sideReal = (mScratchReal[0][(size_t)i] - mScratchReal[1][(size_t)i]) * 0.5;
			const double sideImaginary = (mScratchImaginary[0][(size_t)i] - mScratchImaginary[1][(size_t)i]) * 0.5;
			mScratchReal[0][(size_t)i] = midReal;
			mScratchImaginary[0][(size_t)i] = midImaginary;
			mScratchReal[1][(size_t)i] = sideReal;
			mScratchImaginary[1][(size_t)i] = sideImaginary;
		}
	}

	const int traceCount = mode == (int)AnalyzerChannelMode::Mono ? 1 : 2;
	for (int trace = 0; trace < 2; ++trace) {
		if (trace >= traceCount) {
			std::fill(mSpectrumDb[(size_t)trace].begin(), mSpectrumDb[(size_t)trace].end(), -140.0f);
			std::fill(mSpectrumPeakDb[(size_t)trace].begin(), mSpectrumPeakDb[(size_t)trace].end(), -140.0f);
			continue;
		}

		spectrum::Magnitudes(mScratchReal[trace].data(), mScratchImaginary[trace].data(),
							 kTransformSize, mScratchMagnitude.data());
		spectrum::FoldToLogAxis(mScratchMagnitude.data(), kTransformSize, mSampleRate,
								kMinFrequency, kMaxFrequency, mScratchDb.data(), kSpectrumPoints);

		for (int point = 0; point < kSpectrumPoints; ++point) {
			const float fresh = TiltedDb(mScratchDb[(size_t)point], GetSpectrumPointFrequency(point));

			// NOTE: no smoothing across neighboring points. it reads better on a noise
			// floor and it is a lie about every tone: a peak flanked by quiet neighbors
			// loses several dB of the level the analyzer exists to report
			float& shown = mSpectrumDb[(size_t)trace][(size_t)point];
			shown = fresh > shown ? fresh : shown + (fresh - shown) * kSpectrumRelease;

			float& held = mSpectrumPeakDb[(size_t)trace][(size_t)point];
			held = shown > held ? shown : held - kPeakHoldFallDb;
		}
	}
}

void AnalyzerProcessor::ComputeCrestFactor() {
	// over the whole ring rather than the transform window: a third of a second is long
	// enough to hold a kick and its own decay, which is the ratio the number describes.
	// it is also the one loudness-tab figure that is not published by the audio thread,
	// so it runs on every refresh rather than only for the tabs that draw a spectrum
	double ringPeak = 0.0;
	double ringSquares = 0.0;
	for (int i = 0; i < kRingSize; ++i) {
		const double left = mRingLeft[(size_t)i];
		const double right = mRingRight[(size_t)i];
		ringPeak = std::max(ringPeak, std::max(std::fabs(left), std::fabs(right)));
		ringSquares += left * left + right * right;
	}

	const double ringRMS = std::sqrt(ringSquares / (double)(kRingSize * 2));
	mCrestFactorDb = ringRMS > 1.0e-9 ? (float)(LinearToDb(ringPeak) - LinearToDb(ringRMS)) : 0.0f;
}

void AnalyzerProcessor::ComputeStereoBands() {
	const double rate = mSampleRate > 1.0 ? mSampleRate : 48000.0;
	const double binHz = rate / (double)kTransformSize;
	const double ratio = std::log(kMaxFrequency / kMinFrequency);
	const double scale = 4.0 / (double)kTransformSize;

	for (int band = 0; band < kStereoBands; ++band) {
		StereoBand& out = mStereoBands[(size_t)band];
		out.lowFrequency = kMinFrequency * std::exp(ratio * (double)band / (double)kStereoBands);
		out.highFrequency = kMinFrequency * std::exp(ratio * (double)(band + 1) / (double)kStereoBands);

		const int first = std::max((int)std::floor(out.lowFrequency / binHz), 1);
		const int last = std::max(std::min((int)std::ceil(out.highFrequency / binHz), kTransformSize / 2 - 1), first);

		double sumLeft = 0.0;
		double sumRight = 0.0;
		double sumCross = 0.0;
		double peak = 0.0;
		for (int k = first; k <= last; ++k) {
			const double leftReal = mScratchReal[0][(size_t)k];
			const double leftImaginary = mScratchImaginary[0][(size_t)k];
			const double rightReal = mScratchReal[1][(size_t)k];
			const double rightImaginary = mScratchImaginary[1][(size_t)k];

			const double leftPower = leftReal * leftReal + leftImaginary * leftImaginary;
			const double rightPower = rightReal * rightReal + rightImaginary * rightImaginary;
			sumLeft += leftPower;
			sumRight += rightPower;

			// the real part of L conj(R): the in-phase share of the cross spectrum, which
			// is exactly what a correlation meter reports, resolved per band
			sumCross += leftReal * rightReal + leftImaginary * rightImaginary;

			peak = std::max(peak, std::sqrt(std::max(leftPower, rightPower)) * scale);
		}

		const double denominator = std::sqrt(sumLeft * sumRight);
		out.correlation = denominator > 1.0e-18 ? (float)std::clamp(sumCross / denominator, -1.0, 1.0) : 1.0f;

		// mid and side energy fall straight out of the same three sums, so the width
		// readout costs no extra pass
		const double midPower = std::max((sumLeft + sumRight + 2.0 * sumCross) * 0.25, 0.0);
		const double sidePower = std::max((sumLeft + sumRight - 2.0 * sumCross) * 0.25, 0.0);
		const double total = midPower + sidePower;
		out.widthRatio = total > 1.0e-18 ? (float)(sidePower / total) : 0.0f;
		out.db = (float)LinearToDb(peak);
	}
}

void AnalyzerProcessor::GetStereoBand(int index, double& lowFrequency, double& highFrequency,
									  float& correlation, float& widthRatio, float& db) const {
	const StereoBand& band = mStereoBands[(size_t)std::clamp(index, 0, kStereoBands - 1)];
	lowFrequency = band.lowFrequency;
	highFrequency = band.highFrequency;
	correlation = band.correlation;
	widthRatio = band.widthRatio;
	db = band.db;
}

void AnalyzerProcessor::AdvanceSpectrogram(float deltaSeconds) {
	if (mFrozen)
		return;

	mSpectrogramClock += deltaSeconds;
	if (mSpectrogramClock < kSpectrogramInterval)
		return;

	// a stall drops the backlog rather than fast-forwarding through it; the picture is
	// about what is happening now, not about catching up on a frame that never drew
	mSpectrogramClock = 0.0f;

	float* column = &mSpectrogram[(size_t)mSpectrogramWrite * kSpectrogramRows];
	for (int row = 0; row < kSpectrogramRows; ++row) {
		// each row covers a run of display points, and takes the loudest of them: a
		// narrow tone must survive being squeezed into a quarter of the points
		const int first = row * kSpectrumPoints / kSpectrogramRows;
		const int last = std::max((row + 1) * kSpectrumPoints / kSpectrogramRows, first + 1);
		float level = -140.0f;
		for (int point = first; point < last && point < kSpectrumPoints; ++point)
			level = std::max(level, mSpectrumDb[0][(size_t)point]);
		column[row] = level;
	}
	mSpectrogramWrite = (mSpectrogramWrite + 1) % kSpectrogramColumns;
}

// ================================================================
// LOUDNESS GATING (UI THREAD)
// ================================================================

void AnalyzerProcessor::DrainLoudnessBlocks() {
	const int write = mLoudnessWrite.load(std::memory_order_acquire);

	// fifty seconds of stalled UI would be needed to reach this. skipping past the
	// overwritten entries costs a gap in the integrated figure; reading them would
	// cost a torn one
	if (write - mLoudnessRead > kLoudnessRingSize)
		mLoudnessRead = write - kLoudnessRingSize;

	bool addedIntegrated = false;
	bool addedRange = false;
	while (mLoudnessRead < write) {
		const LoudnessBlock& block = mLoudnessRing[(size_t)(mLoudnessRead & (kLoudnessRingSize - 1))];
		if ((int)mIntegratedPower.size() < kMaxLoudnessBlocks) {
			mIntegratedPower.push_back(block.momentaryPower);
			addedIntegrated = true;
			if (block.shortTermValid) {
				mRangePower.push_back(block.shortTermPower);
				addedRange = true;
			}
		}
		++mLoudnessRead;
	}

	if (addedIntegrated)
		RecomputeIntegratedLoudness();

	// the range needs a sort, so it is recomputed only when a block actually arrived -
	// ten times a second, not sixty
	if (addedRange)
		RecomputeLoudnessRange();
}

void AnalyzerProcessor::RecomputeIntegratedLoudness() {
	// two gates, in the order BS.1770-4 specifies them. without the relative one a
	// quiet intro drags the figure for the whole take down with it
	double sum = 0.0;
	int count = 0;
	for (float power : mIntegratedPower) {
		if (PowerToLoudness(power) > kAbsoluteGateLoudness) {
			sum += power;
			++count;
		}
	}
	if (count == 0) {
		mIntegratedLoudness = -100.0f;
		return;
	}

	const double relativeGate = PowerToLoudness(sum / (double)count) - kIntegratedRelativeGate;
	sum = 0.0;
	count = 0;
	for (float power : mIntegratedPower) {
		const double loudness = PowerToLoudness(power);
		if (loudness > kAbsoluteGateLoudness && loudness > relativeGate) {
			sum += power;
			++count;
		}
	}

	mIntegratedLoudness = count > 0 ? (float)PowerToLoudness(sum / (double)count) : -100.0f;
}

void AnalyzerProcessor::RecomputeLoudnessRange() {
	static std::vector<float> gated;
	gated.clear();

	double sum = 0.0;
	int count = 0;
	for (float power : mRangePower) {
		if (PowerToLoudness(power) > kAbsoluteGateLoudness) {
			sum += power;
			++count;
		}
	}
	if (count == 0) {
		mLoudnessRange = 0.0f;
		return;
	}

	const double relativeGate = PowerToLoudness(sum / (double)count) - kRangeRelativeGate;
	for (float power : mRangePower) {
		const double loudness = PowerToLoudness(power);
		if (loudness > kAbsoluteGateLoudness && loudness > relativeGate)
			gated.push_back((float)loudness);
	}
	if (gated.size() < 2) {
		mLoudnessRange = 0.0f;
		return;
	}

	std::sort(gated.begin(), gated.end());
	const int lastIndex = (int)gated.size() - 1;
	const int low = std::clamp((int)std::lround(kRangeLowPercentile * lastIndex), 0, lastIndex);
	const int high = std::clamp((int)std::lround(kRangeHighPercentile * lastIndex), 0, lastIndex);
	mLoudnessRange = std::max(gated[(size_t)high] - gated[(size_t)low], 0.0f);
}

// ================================================================
// READOUT
// ================================================================

float AnalyzerProcessor::GetPeakDb(int channel) const {
	return (float)LinearToDb(mPeak[(size_t)std::clamp(channel, 0, 1)].load(std::memory_order_relaxed));
}

float AnalyzerProcessor::GetTruePeakDb(int channel) const {
	return (float)LinearToDb(mTruePeak[(size_t)std::clamp(channel, 0, 1)].load(std::memory_order_relaxed));
}

float AnalyzerProcessor::GetRMSDb(int channel) const {
	return (float)LinearToDb(mRMS[(size_t)std::clamp(channel, 0, 1)].load(std::memory_order_relaxed));
}

float AnalyzerProcessor::GetOffset(int channel) const {
	return mOffset[(size_t)std::clamp(channel, 0, 1)].load(std::memory_order_relaxed);
}

float AnalyzerProcessor::GetCorrelation() const {
	return mCorrelation.load(std::memory_order_relaxed);
}

float AnalyzerProcessor::GetMomentaryLoudness() const {
	return mMomentaryLoudness.load(std::memory_order_relaxed);
}

float AnalyzerProcessor::GetShortTermLoudness() const {
	return mShortTermLoudness.load(std::memory_order_relaxed);
}

// ================================================================
// SERIALIZATION
// ================================================================

void AnalyzerProcessor::Save(std::ostream& out) {
	// written before the inherited PARAMS block: AudioProcessor::Load stops at
	// PARAMS_END and Track::Load eats exactly one line after it, so there is no room
	// for extra lines on the far side
	out << "ANALYZER_VIEW " << mView << " " << mChannelMode << " " << mTriggerMode << "\n";
	out << "ANALYZER_SCALE " << mTiltDbPerOctave << " " << mFloorDb << " " << mCeilingDb << " "
		<< mScopeWindow.value << " " << (mShowPeakHold ? 1 : 0) << "\n";
	AudioProcessor::Save(out);
}

void AnalyzerProcessor::Load(std::istream& in) {
	std::string line;

	while (std::getline(in, line)) {
		if (line.rfind("ANALYZER_VIEW ", 0) == 0) {
			std::stringstream stream(line.substr(14));
			stream >> mView >> mChannelMode >> mTriggerMode;
		} else if (line.rfind("ANALYZER_SCALE ", 0) == 0) {
			std::stringstream stream(line.substr(15));
			int peakHold = 1;
			stream >> mTiltDbPerOctave >> mFloorDb >> mCeilingDb >> mScopeWindow.value >> peakHold;
			mShowPeakHold = peakHold != 0;
		} else {
			// anything else is the base class's territory. it tolerates the leading
			// line we just consumed, and reads on to PARAMS_END
			AudioProcessor::Load(in);
			break;
		}
	}

	mView = std::clamp(mView, 0, (int)AnalyzerView::Count - 1);
	mChannelMode = std::clamp(mChannelMode, 0, (int)AnalyzerChannelMode::Count - 1);
	mTriggerMode = std::clamp(mTriggerMode, 0, (int)AnalyzerTriggerMode::Count - 1);
	mTiltDbPerOctave = std::clamp(mTiltDbPerOctave, 0.0f, 6.0f);
	mFloorDb = std::clamp(mFloorDb, -140.0f, -24.0f);
	mCeilingDb = std::clamp(mCeilingDb, -12.0f, 24.0f);
	mScopeWindow.value = std::clamp(mScopeWindow.value, 1.0f, 200.0f);
}

void AnalyzerProcessor::CopyStateFrom(const AudioProcessor& other) {
	const auto* source = dynamic_cast<const AnalyzerProcessor*>(&other);
	if (!source)
		return;

	// only the view survives a duplicate. the measurements belong to the signal that
	// went through the original, and carrying them over would be a lie about the copy
	mView = source->mView;
	mChannelMode = source->mChannelMode;
	mTriggerMode = source->mTriggerMode;
	mTiltDbPerOctave = source->mTiltDbPerOctave;
	mFloorDb = source->mFloorDb;
	mCeilingDb = source->mCeilingDb;
	mScopeWindow.value = source->mScopeWindow.value;
	mShowPeakHold = source->mShowPeakHold;
}

// ================================================================
// UI - SHARED PIECES
// ================================================================

void AnalyzerProcessor::DrawFrequencyGrid(const ImVec2& pos, const ImVec2& size, bool labels) const {
	const Theme& th = Theme::Instance();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const double span = std::log(kMaxFrequency / kMinFrequency);

	for (const GridLine& line : kGridLines) {
		const float x = pos.x + size.x * (float)(std::log(line.frequency / kMinFrequency) / span);
		if (x < pos.x || x > pos.x + size.x)
			continue;

		dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y),
					line.label ? th.gridBeat : th.gridSub, 1.0f);
		if (labels && line.label) {
			dl->AddText(ImVec2(x + 3.0f, pos.y + size.y - ImGui::GetTextLineHeight() - 2.0f),
						th.textDim, line.label);
		}
	}
}

void AnalyzerProcessor::DrawReadout(const ImVec2& pos, const ImVec2& size, const char* label,
									const char* value, float fill, unsigned int fillColor) const {
	const Theme& th = Theme::Instance();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 end(pos.x + size.x, pos.y + size.y);

	dl->AddRectFilled(pos, end, th.bgDeepest, ImGui::GetStyle().FrameRounding);
	dl->AddText(ImVec2(pos.x + 5.0f, pos.y + 3.0f), th.textMuted, label);
	dl->AddText(ImVec2(pos.x + 5.0f, pos.y + 3.0f + ImGui::GetTextLineHeight()), th.text, value);

	// the bar is a shape to glance at, not a second copy of the number: it only has to
	// say high, low, or off the end
	const float barTop = end.y - 6.0f;
	const float barLeft = pos.x + 5.0f;
	const float barRight = end.x - 5.0f;
	dl->AddRectFilled(ImVec2(barLeft, barTop), ImVec2(barRight, barTop + 3.0f), th.meterBg);
	if (fill > 0.0f) {
		const float width = (barRight - barLeft) * std::clamp(fill, 0.0f, 1.0f);
		dl->AddRectFilled(ImVec2(barLeft, barTop), ImVec2(barLeft + width, barTop + 3.0f), fillColor);
	}
}

// ================================================================
// UI - SPECTRUM
// ================================================================

void AnalyzerProcessor::DrawSpectrum(const ImVec2& pos, const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 end(pos.x + size.x, pos.y + size.y);
	const double span = std::log(kMaxFrequency / kMinFrequency);
	const float range = std::max(mCeilingDb - mFloorDb, 1.0f);

	auto DbToY = [&](float db) {
		return pos.y + size.y * (1.0f - std::clamp((db - mFloorDb) / range, 0.0f, 1.0f));
	};

	dl->AddRectFilled(pos, end, th.bgDeepest);
	dl->PushClipRect(pos, end, true);

	// ---- rules ----
	for (float db = std::floor(mCeilingDb / 12.0f) * 12.0f; db > mFloorDb; db -= 12.0f) {
		const float y = DbToY(db);
		dl->AddLine(ImVec2(pos.x, y), ImVec2(end.x, y), th.gridSub, 1.0f);

		char label[16];
		snprintf(label, sizeof(label), "%+.0f", db);
		dl->AddText(ImVec2(pos.x + 3.0f, y + 1.0f), th.textDim, label);
	}
	DrawFrequencyGrid(pos, size, true);

	// ---- traces ----
	// trace 1 is drawn first so trace 0 (left / mid / mono) reads on top of it
	const int traceCount = mChannelMode == (int)AnalyzerChannelMode::Mono ? 1 : 2;
	for (int trace = traceCount - 1; trace >= 0; --trace) {
		const ImU32 fill = trace == 0 ? th.spectrumFill : th.spectrumFillAlt;
		const ImU32 edge = trace == 0 ? th.spectrumEdge : th.spectrumEdgeAlt;

		ImVec2 previous(pos.x, end.y);
		for (int point = 0; point < kSpectrumPoints; ++point) {
			const float x = pos.x + size.x * ((float)point / (float)(kSpectrumPoints - 1));
			const ImVec2 current(x, DbToY(mSpectrumDb[(size_t)trace][(size_t)point]));

			if (point > 0) {
				dl->AddQuadFilled(previous, current, ImVec2(current.x, end.y), ImVec2(previous.x, end.y), fill);
				dl->AddLine(previous, current, edge, 1.0f);
			}
			previous = current;
		}

		if (mShowPeakHold) {
			ImVec2 held(pos.x, end.y);
			for (int point = 0; point < kSpectrumPoints; ++point) {
				const float x = pos.x + size.x * ((float)point / (float)(kSpectrumPoints - 1));
				const ImVec2 current(x, DbToY(mSpectrumPeakDb[(size_t)trace][(size_t)point]));
				if (point > 0)
					dl->AddLine(held, current, Theme::WithAlpha(edge, 200), 1.0f);
				held = current;
			}
		}
	}

	// ---- cursor readout ----
	// the note name is the point of this: a resonance at 155 Hz is a number, a
	// resonance on your D#3 is something you can go and fix in the piano roll
	if (ImGui::IsMouseHoveringRect(pos, end)) {
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const double t = std::clamp((double)(mouse.x - pos.x) / (double)size.x, 0.0, 1.0);
		const double frequency = kMinFrequency * std::exp(span * t);
		const int nearest = std::clamp((int)std::lround(t * kSpectrumPoints - 0.5), 0, kSpectrumPoints - 1);

		dl->AddLine(ImVec2(mouse.x, pos.y), ImVec2(mouse.x, end.y), th.accentTranslucent, 1.0f);

		char frequencyText[32];
		char noteText[32];
		char text[128];
		FormatFrequency(frequency, frequencyText, sizeof(frequencyText));
		FrequencyToNote(frequency, noteText, sizeof(noteText));
		snprintf(text, sizeof(text), "%s   %s   %.1f dB", frequencyText, noteText,
				 mSpectrumDb[0][(size_t)nearest]);

		const ImVec2 textSize = ImGui::CalcTextSize(text);
		const ImVec2 boxMin(std::min(mouse.x + 10.0f, end.x - textSize.x - 12.0f), pos.y + 4.0f);
		dl->AddRectFilled(boxMin - ImVec2(4.0f, 3.0f), boxMin + textSize + ImVec2(4.0f, 3.0f),
						  th.bgOverlay, ImGui::GetStyle().FrameRounding);
		dl->AddText(boxMin, th.text, text);
	}

	dl->PopClipRect();
	dl->AddRect(pos, end, th.border);
}

// ================================================================
// UI - SPECTROGRAM
// ================================================================

void AnalyzerProcessor::DrawSpectrogram(const ImVec2& pos, const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 end(pos.x + size.x, pos.y + size.y);
	const float range = std::max(mCeilingDb - mFloorDb, 1.0f);

	dl->AddRectFilled(pos, end, th.spectrogramFloor);
	dl->PushClipRect(pos, end, true);

	// the level is quantized before it becomes a color so that runs of equal cells
	// merge into one rectangle. an unquantized ramp would emit one quad per cell, and
	// there are twenty-five thousand of them
	auto ColorFor = [&](float db) {
		const float norm = std::clamp((db - mFloorDb) / range, 0.0f, 1.0f);
		const int step = (int)(norm * 31.0f);
		return th.SpectrogramColor((float)step / 31.0f);
	};

	const float cellWidth = size.x / (float)kSpectrogramColumns;
	const float cellHeight = size.y / (float)kSpectrogramRows;

	for (int column = 0; column < kSpectrogramColumns; ++column) {
		// the oldest column sits at the left, so time runs the same way as the timeline
		const int index = (mSpectrogramWrite + column) % kSpectrogramColumns;
		const float* cells = &mSpectrogram[(size_t)index * kSpectrogramRows];
		const float x = pos.x + (float)column * cellWidth;

		int runStart = 0;
		ImU32 runColor = ColorFor(cells[0]);
		for (int row = 1; row <= kSpectrogramRows; ++row) {
			const ImU32 color = row < kSpectrogramRows ? ColorFor(cells[row]) : 0u;
			if (row < kSpectrogramRows && color == runColor)
				continue;

			// row 0 is the lowest frequency, so the picture is built from the bottom up
			const float top = pos.y + size.y - (float)row * cellHeight;
			const float bottom = pos.y + size.y - (float)runStart * cellHeight;
			dl->AddRectFilled(ImVec2(x, top), ImVec2(x + cellWidth + 1.0f, bottom), runColor);

			runStart = row;
			runColor = color;
		}
	}

	// only the decades are labelled here: the picture is the point, and a full grid
	// over it would fight the thing it is meant to help read
	const double span = std::log(kMaxFrequency / kMinFrequency);
	const double marks[] = {100.0, 1000.0, 10000.0};
	const char* markLabels[] = {"100", "1k", "10k"};
	for (int mark = 0; mark < 3; ++mark) {
		const float y = pos.y + size.y * (1.0f - (float)(std::log(marks[mark] / kMinFrequency) / span));
		dl->AddLine(ImVec2(pos.x, y), ImVec2(end.x, y), Theme::WithAlpha(th.gridSub, 90), 1.0f);
		dl->AddText(ImVec2(pos.x + 3.0f, y - ImGui::GetTextLineHeight() - 1.0f), th.textDim, markLabels[mark]);
	}

	dl->PopClipRect();
	dl->AddRect(pos, end, th.border);
}

// ================================================================
// UI - STEREO
// ================================================================

void AnalyzerProcessor::DrawStereo(const ImVec2& pos, const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImGuiStyle& style = ImGui::GetStyle();

	// the goniometer is square, and everything else gets the width it leaves
	const float scopeSide = std::min(size.y, size.x * 0.42f);
	const ImVec2 scopePos(pos.x, pos.y);
	const ImVec2 scopeEnd(pos.x + scopeSide, pos.y + scopeSide);
	const ImVec2 center((scopePos.x + scopeEnd.x) * 0.5f, (scopePos.y + scopeEnd.y) * 0.5f);
	const float radius = scopeSide * 0.5f - 2.0f;

	dl->AddRectFilled(scopePos, scopeEnd, th.bgDeepest);
	dl->PushClipRect(scopePos, scopeEnd, true);

	// the circle and its diagonals: vertical is mono, horizontal is pure side, and each
	// diagonal is one channel on its own
	dl->AddCircle(center, radius, th.gridSub, 48, 1.0f);
	dl->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), th.gridSub, 1.0f);
	dl->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), th.gridSub, 1.0f);
	const float diagonal = radius * 0.7071f;
	dl->AddLine(ImVec2(center.x - diagonal, center.y - diagonal),
				ImVec2(center.x + diagonal, center.y + diagonal), th.gridSub, 1.0f);
	dl->AddLine(ImVec2(center.x + diagonal, center.y - diagonal),
				ImVec2(center.x - diagonal, center.y + diagonal), th.gridSub, 1.0f);
	dl->AddText(ImVec2(scopePos.x + 3.0f, scopePos.y + 2.0f), th.textDim, "L");
	dl->AddText(ImVec2(scopeEnd.x - ImGui::CalcTextSize("R").x - 3.0f, scopePos.y + 2.0f), th.textDim, "R");

	// ---- the trace ----
	// rotated 45 degrees, the standard orientation: up is what survives a mono sum and
	// sideways is what cancels in it. older samples fade so the newest shape leads
	constexpr int kTracePoints = 2048;
	const int write = mRingWrite.load(std::memory_order_relaxed);
	for (int i = 0; i < kTracePoints; ++i) {
		const int index = (write - kTracePoints + i) & (kRingSize - 1);
		const float left = mRingLeft[(size_t)index];
		const float right = mRingRight[(size_t)index];

		const float side = (left - right) * 0.7071f;
		const float mid = (left + right) * 0.7071f;
		const ImVec2 point(center.x + std::clamp(side, -1.0f, 1.0f) * radius,
						   center.y - std::clamp(mid, -1.0f, 1.0f) * radius);

		const int alpha = 30 + (int)(170.0f * ((float)i / (float)kTracePoints));
		dl->AddRectFilled(point, ImVec2(point.x + 1.5f, point.y + 1.5f),
						  Theme::WithAlpha(th.graphCurve, alpha));
	}

	dl->PopClipRect();
	dl->AddRect(scopePos, scopeEnd, th.border);

	// ---- overall correlation ----
	const float correlation = GetCorrelation();
	const float meterTop = scopeEnd.y + style.ItemSpacing.y;
	const float meterHeight = std::max(size.y - scopeSide - style.ItemSpacing.y, 0.0f);
	if (meterHeight >= 10.0f) {
		const ImVec2 meterPos(scopePos.x, meterTop);
		const ImVec2 meterEnd(scopeEnd.x, meterTop + std::min(meterHeight, 16.0f));
		dl->AddRectFilled(meterPos, meterEnd, th.meterBg);

		const float middle = (meterPos.x + meterEnd.x) * 0.5f;
		const float half = (meterEnd.x - meterPos.x) * 0.5f;
		const float marker = middle + correlation * half;
		const ImU32 color = correlation < 0.0f ? th.danger : (correlation < 0.5f ? th.meterMid : th.success);
		dl->AddRectFilled(ImVec2(std::min(middle, marker), meterPos.y),
						  ImVec2(std::max(middle, marker), meterEnd.y), color);
		dl->AddLine(ImVec2(middle, meterPos.y), ImVec2(middle, meterEnd.y), th.borderStrong, 1.0f);

		char text[32];
		snprintf(text, sizeof(text), "%+.2f", correlation);
		dl->AddText(ImVec2(meterPos.x + 4.0f, meterPos.y + 1.0f), th.text, text);
	}

	// ---- per band ----
	// this is the readout the tab exists for: it says which part of the spectrum is
	// wide and which is centered, instead of averaging the two into one number that
	// hides a mono top end behind a wide pad
	const ImVec2 bandsPos(scopeEnd.x + style.ItemSpacing.x, pos.y);
	const ImVec2 bandsEnd(pos.x + size.x, pos.y + size.y);
	if (bandsEnd.x - bandsPos.x < 40.0f)
		return;

	dl->AddRectFilled(bandsPos, bandsEnd, th.bgDeepest);
	dl->PushClipRect(bandsPos, bandsEnd, true);

	const float rowHeight = (bandsEnd.y - bandsPos.y) / (float)kStereoBands;
	const float labelWidth = ImGui::CalcTextSize("10k").x + 8.0f;
	const float barLeft = bandsPos.x + labelWidth;
	const float barMiddle = (barLeft + bandsEnd.x) * 0.5f;
	const float barHalf = (bandsEnd.x - barLeft) * 0.5f - 3.0f;

	dl->AddLine(ImVec2(barMiddle, bandsPos.y), ImVec2(barMiddle, bandsEnd.y), th.gridBeat, 1.0f);

	for (int band = 0; band < kStereoBands; ++band) {
		const StereoBand& data = mStereoBands[(size_t)band];
		const float top = bandsPos.y + (float)band * rowHeight + 1.0f;
		const float bottom = top + rowHeight - 2.0f;

		// a band with nothing in it has no phase to report, so it fades out instead of
		// drawing a confident bar over silence
		const float level = std::clamp((data.db - mFloorDb) / std::max(mCeilingDb - mFloorDb, 1.0f), 0.0f, 1.0f);
		const int alpha = 25 + (int)(215.0f * level);

		const float marker = barMiddle + data.correlation * barHalf;
		const ImU32 color = data.correlation < 0.0f ? th.danger
													: (data.correlation < 0.5f ? th.meterMid : th.success);
		dl->AddRectFilled(ImVec2(std::min(barMiddle, marker), top),
						  ImVec2(std::max(barMiddle, marker), bottom), Theme::WithAlpha(color, alpha));

		char label[16];
		if (data.highFrequency >= 1000.0)
			snprintf(label, sizeof(label), "%.0fk", data.highFrequency / 1000.0);
		else
			snprintf(label, sizeof(label), "%.0f", data.highFrequency);
		dl->AddText(ImVec2(bandsPos.x + 3.0f, top), th.textDim, label);
	}

	const char* wide = "wide";
	const char* mono = "mono";
	const float legendY = bandsEnd.y - ImGui::GetTextLineHeight() - 1.0f;
	dl->AddText(ImVec2(barLeft + 2.0f, legendY), th.textDim, wide);
	dl->AddText(ImVec2(bandsEnd.x - ImGui::CalcTextSize(mono).x - 3.0f, legendY), th.textDim, mono);

	dl->PopClipRect();
	dl->AddRect(bandsPos, bandsEnd, th.border);
}

// ================================================================
// UI - LOUDNESS
// ================================================================

void AnalyzerProcessor::DrawLoudness(const ImVec2& pos, const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImGuiStyle& style = ImGui::GetStyle();

	constexpr int kColumns = 4;
	constexpr int kRows = 3;
	const float cellWidth = (size.x - style.ItemSpacing.x * (kColumns - 1)) / (float)kColumns;
	const float cellHeight = (size.y - style.ItemSpacing.y * (kRows - 1)) / (float)kRows;
	const ImVec2 cellSize(cellWidth, cellHeight);

	auto Cell = [&](int column, int row) {
		return ImVec2(pos.x + (cellWidth + style.ItemSpacing.x) * (float)column,
					  pos.y + (cellHeight + style.ItemSpacing.y) * (float)row);
	};

	// loudness bars fill from -40 LUFS, which puts the streaming targets in the top
	// third; peak bars fill from -60 dBFS so the last few dB stay readable
	auto LoudnessFill = [](float loudness) {
		return std::clamp((loudness + 40.0f) / 40.0f, 0.0f, 1.0f);
	};
	auto PeakFill = [](float db) {
		return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
	};

	char text[48];

	auto Loudness = [&](int column, int row, const char* label, float value) {
		if (value <= -99.0f)
			snprintf(text, sizeof(text), "--");
		else
			snprintf(text, sizeof(text), "%.1f", value);
		DrawReadout(Cell(column, row), cellSize, label, text, LoudnessFill(value),
					th.MeterColor(LoudnessFill(value)));
	};

	auto Peak = [&](int column, int row, const char* label, float db, bool warnNearFullScale) {
		snprintf(text, sizeof(text), "%.1f", db);
		// -1 dBFS true peak is where a lossy encoder starts clipping on playback, so it
		// reads red well before the meter itself runs out of room
		const ImU32 color = (warnNearFullScale && db > -1.0f) ? th.danger : th.MeterColor(PeakFill(db));
		DrawReadout(Cell(column, row), cellSize, label, text, PeakFill(db), color);
	};

	Loudness(0, 0, "MOMENTARY LUFS", GetMomentaryLoudness());
	Loudness(1, 0, "SHORT TERM LUFS", GetShortTermLoudness());
	Loudness(2, 0, "INTEGRATED LUFS", GetIntegratedLoudness());

	snprintf(text, sizeof(text), "%.1f", mLoudnessRange);
	DrawReadout(Cell(3, 0), cellSize, "RANGE LU", text,
				std::clamp(mLoudnessRange / 20.0f, 0.0f, 1.0f), th.accent);

	Peak(0, 1, "PEAK L", GetPeakDb(0), false);
	Peak(1, 1, "PEAK R", GetPeakDb(1), false);
	Peak(2, 1, "TRUE PEAK L", GetTruePeakDb(0), true);
	Peak(3, 1, "TRUE PEAK R", GetTruePeakDb(1), true);

	Peak(0, 2, "RMS L", GetRMSDb(0), false);
	Peak(1, 2, "RMS R", GetRMSDb(1), false);

	snprintf(text, sizeof(text), "%.1f", GetCrestFactorDb());
	DrawReadout(Cell(2, 2), cellSize, "CREST dB", text,
				std::clamp(GetCrestFactorDb() / 24.0f, 0.0f, 1.0f), th.graphCurveCool);

	// dc offset costs headroom and nothing else in the DAW reports it. a hundredth of
	// full scale is where it stops being rounding noise and starts being a bug upstream
	const float offset = std::max(std::fabs(GetOffset(0)), std::fabs(GetOffset(1)));
	snprintf(text, sizeof(text), "%.4f", offset);
	DrawReadout(Cell(3, 2), cellSize, "DC OFFSET", text, std::clamp(offset * 100.0f, 0.0f, 1.0f),
				offset > 0.01f ? th.danger : th.success);
}

// ================================================================
// UI - SCOPE
// ================================================================

void AnalyzerProcessor::DrawScope(const ImVec2& pos, const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 end(pos.x + size.x, pos.y + size.y);

	dl->AddRectFilled(pos, end, th.bgDeepest);
	dl->PushClipRect(pos, end, true);

	const float middle = pos.y + size.y * 0.5f;
	dl->AddLine(ImVec2(pos.x, middle), ImVec2(end.x, middle), th.gridBeat, 1.0f);
	for (int division = 1; division < 4; ++division) {
		const float x = pos.x + size.x * ((float)division / 4.0f);
		dl->AddLine(ImVec2(x, pos.y), ImVec2(x, end.y), th.gridSub, 1.0f);
	}

	const int write = mRingWrite.load(std::memory_order_relaxed);
	const int windowSamples = std::clamp((int)(mScopeWindow.value * 0.001f * (float)mSampleRate), 16, kRingSize / 2);
	int start = write - windowSamples;

	if (mTriggerMode == (int)AnalyzerTriggerMode::Rising) {
		// walk back from the newest window looking for an upward zero crossing, so a
		// steady tone stands still instead of sliding across the graph
		for (int back = 0; back < kRingSize / 4; ++back) {
			const int index = (start - back) & (kRingSize - 1);
			const int previous = (index - 1) & (kRingSize - 1);
			const float current = mRingLeft[(size_t)index] + mRingRight[(size_t)index];
			const float before = mRingLeft[(size_t)previous] + mRingRight[(size_t)previous];
			if (before <= 0.0f && current > 0.0f) {
				start -= back;
				break;
			}
		}
	} else if (mTriggerMode == (int)AnalyzerTriggerMode::Tempo) {
		// line the window up with the last beat boundary far enough back for the whole
		// window to fit, so a one-bar loop draws the same picture on every pass
		const double samplesPerBeat = std::max(mSamplesPerBeat.load(std::memory_order_relaxed), 1.0);
		const int64_t newest = mRingSample.load(std::memory_order_relaxed);
		const int64_t latest = newest - windowSamples;
		if (latest > 0) {
			const int64_t boundary = (int64_t)(std::floor((double)latest / samplesPerBeat) * samplesPerBeat);
			const int64_t back = newest - boundary;
			if (back > 0 && back < kRingSize)
				start = write - (int)back;
		}
	}

	// one vertical span per pixel column: at 20 ms and 400 pixels that is a couple of
	// samples each, and at 200 ms it becomes a proper min/max envelope
	const int columns = std::max((int)size.x, 1);
	for (int channel = 0; channel < 2; ++channel) {
		const std::vector<float>& ring = channel == 0 ? mRingLeft : mRingRight;
		const ImU32 color = channel == 0 ? th.graphCurve : th.graphCurveCool;

		for (int column = 0; column < columns; ++column) {
			const int first = start + windowSamples * column / columns;
			const int last = std::max(start + windowSamples * (column + 1) / columns, first + 1);

			float low = 1.0f;
			float high = -1.0f;
			for (int i = first; i < last; ++i) {
				const float sample = ring[(size_t)(i & (kRingSize - 1))];
				low = std::min(low, sample);
				high = std::max(high, sample);
			}

			const float x = pos.x + (float)column;
			const float top = middle - std::clamp(high, -1.0f, 1.0f) * size.y * 0.5f;
			const float bottom = middle - std::clamp(low, -1.0f, 1.0f) * size.y * 0.5f;
			dl->AddLine(ImVec2(x, top), ImVec2(x, std::max(bottom, top + 1.0f)),
						Theme::WithAlpha(color, 200), 1.0f);
		}
	}

	char text[48];
	snprintf(text, sizeof(text), "%.0f ms", mScopeWindow.value);
	dl->AddText(ImVec2(pos.x + 4.0f, pos.y + 2.0f), th.textDim, text);

	dl->PopClipRect();
	dl->AddRect(pos, end, th.border);
}

// ================================================================
// UI - CONTROLS AND LAYOUT
// ================================================================

void AnalyzerProcessor::DrawControlRow(float width) {
	const Theme& th = Theme::Instance();
	ImGuiStyle& style = ImGui::GetStyle();

	// Freeze and Reset are pinned to the right on every tab, so the two controls that
	// apply to all of them never move when the tab changes
	const float freezeWidth = ImGui::CalcTextSize("Freeze").x + style.FramePadding.x * 2.0f;
	const float resetWidth = ImGui::CalcTextSize("Reset").x + style.FramePadding.x * 2.0f;
	const float rightWidth = freezeWidth + resetWidth + style.ItemSpacing.x;
	const float leftWidth = std::max(width - rightWidth - style.ItemSpacing.x, 60.0f);

	ImGui::BeginGroup();
	const ImVec2 rowStart = ImGui::GetCursorScreenPos();

	auto Combo = [&](const char* id, const char* preview, const char* const* names, int count, int& value, const char* tooltip) {
		ImGui::SetNextItemWidth(std::min(leftWidth * 0.3f, 92.0f));
		if (ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightSmall)) {
			for (int i = 0; i < count; ++i) {
				if (ImGui::Selectable(names[i], i == value))
					value = i;
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);
		ImGui::SameLine();
	};

	switch ((AnalyzerView)mView) {
	case AnalyzerView::Spectrum: {
		Combo("##Channels", kChannelModeNames[std::clamp(mChannelMode, 0, (int)AnalyzerChannelMode::Count - 1)],
			  kChannelModeNames, (int)AnalyzerChannelMode::Count, mChannelMode,
			  "L / R draws both channels, M / S draws the mono sum against what only\n"
			  "exists in the sides. M / S is how you see whether the low end is centered");

		char tiltLabel[32];
		snprintf(tiltLabel, sizeof(tiltLabel), "%.1f dB/oct", mTiltDbPerOctave);
		ImGui::SetNextItemWidth(std::min(leftWidth * 0.32f, 104.0f));
		if (ImGui::BeginCombo("##Tilt", tiltLabel, ImGuiComboFlags_HeightSmall)) {
			const float tilts[] = {0.0f, 3.0f, 4.5f, 6.0f};
			for (float tilt : tilts) {
				char entry[32];
				snprintf(entry, sizeof(entry), "%.1f dB/oct", tilt);
				if (ImGui::Selectable(entry, mTiltDbPerOctave == tilt))
					mTiltDbPerOctave = tilt;
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Rotates the curve about 1 kHz. Real music falls off toward the top,\n"
							  "so a tilt of 3 to 4.5 dB/oct makes a balanced mix read roughly flat\n"
							  "and turns the graph into something you can compare against");
		}
		ImGui::SameLine();

		ImGui::Checkbox("Peak", &mShowPeakHold);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Hold the highest level each band has reached, falling slowly");
		break;
	}
	case AnalyzerView::Spectrogram: {
		char floorLabel[32];
		snprintf(floorLabel, sizeof(floorLabel), "Floor %.0f dB", mFloorDb);
		ImGui::SetNextItemWidth(std::min(leftWidth * 0.36f, 120.0f));
		if (ImGui::BeginCombo("##Floor", floorLabel, ImGuiComboFlags_HeightSmall)) {
			const float floors[] = {-60.0f, -72.0f, -96.0f, -120.0f};
			for (float value : floors) {
				char entry[32];
				snprintf(entry, sizeof(entry), "Floor %.0f dB", value);
				if (ImGui::Selectable(entry, mFloorDb == value))
					mFloorDb = value;
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Where the color ramp bottoms out. Raise it to see only what is loud");
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("time runs left to right");
		ImGui::SameLine();
		break;
	}
	case AnalyzerView::Stereo:
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("above center = mono safe, sideways = cancels in mono");
		ImGui::SameLine();
		break;
	case AnalyzerView::Loudness:
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("integrated and range measure from the last reset");
		ImGui::SameLine();
		break;
	case AnalyzerView::Scope: {
		Combo("##Trigger", kTriggerNames[std::clamp(mTriggerMode, 0, (int)AnalyzerTriggerMode::Count - 1)],
			  kTriggerNames, (int)AnalyzerTriggerMode::Count, mTriggerMode,
			  "Free runs, Edge locks to a rising zero crossing so a tone stands still,\n"
			  "Beat locks the window to the transport so a loop draws the same picture");

		// null format: the parameter prints its own ms units, so this box reads the same
		// as every other millisecond control in the app
		mScopeWindow.DrawCompact(std::min(leftWidth * 0.4f, 130.0f), nullptr);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How much time the width of the graph covers");
		ImGui::SameLine();
		break;
	}
	default:
		break;
	}

	ImGui::EndGroup();

	// park the two shared buttons at the right edge regardless of what the tab put on
	// the left, and never let a long tab row push them off the panel
	ImGui::SetCursorScreenPos(ImVec2(rowStart.x + width - rightWidth, rowStart.y));

	if (mFrozen)
		ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
	if (ImGui::Button("Freeze", ImVec2(freezeWidth, 0.0f)))
		mFrozen = !mFrozen;
	if (mFrozen)
		ImGui::PopStyleColor();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Hold the picture still so it can be read, or compared against another track");

	ImGui::SameLine();
	if (ImGui::Button("Reset", ImVec2(resetWidth, 0.0f)))
		ResetMeasurements();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Clear the held peaks, the integrated loudness and the sonogram");
}

bool AnalyzerProcessor::RenderCustomUI(const ImVec2& size) {
	const Theme& th = Theme::Instance();
	ImGuiStyle& style = ImGui::GetStyle();

	Refresh();

	// the history only advances while the tab is up. letting it run in the background
	// would fill it with whatever the spectrum happened to be frozen at
	if ((AnalyzerView)mView == AnalyzerView::Spectrogram)
		AdvanceSpectrogram(ImGui::GetIO().DeltaTime);

	// ---- tabs ----
	const int viewCount = (int)AnalyzerView::Count;
	const float tabWidth = (size.x - style.ItemSpacing.x * (float)(viewCount - 1)) / (float)viewCount;
	for (int view = 0; view < viewCount; ++view) {
		if (view > 0)
			ImGui::SameLine();

		const bool active = mView == view;
		if (active) {
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accentHover);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}
		if (ImGui::Button(kViewNames[view], ImVec2(tabWidth, 0.0f)))
			mView = view;
		if (active)
			ImGui::PopStyleColor(3);
	}

	// ---- body ----
	const float rowHeight = ImGui::GetFrameHeight();
	const float bodyHeight = std::max(size.y - rowHeight * 2.0f - style.ItemSpacing.y * 2.0f, 60.0f);
	const ImVec2 bodyPos = ImGui::GetCursorScreenPos();
	const ImVec2 bodySize(size.x, bodyHeight);
	ImGui::Dummy(bodySize);

	switch ((AnalyzerView)mView) {
	case AnalyzerView::Spectrum:
		DrawSpectrum(bodyPos, bodySize);
		break;
	case AnalyzerView::Spectrogram:
		DrawSpectrogram(bodyPos, bodySize);
		break;
	case AnalyzerView::Stereo:
		DrawStereo(bodyPos, bodySize);
		break;
	case AnalyzerView::Loudness:
		DrawLoudness(bodyPos, bodySize);
		break;
	case AnalyzerView::Scope:
		DrawScope(bodyPos, bodySize);
		break;
	default:
		break;
	}

	DrawControlRow(size.x);
	return true;
}
