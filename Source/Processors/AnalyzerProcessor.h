#pragma once
#include "AudioProcessor.h"
#include "Parameters/KnobParameter.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

// which readout the device is showing. the value is serialized, so entries are
// never reordered
enum class AnalyzerView {
	Spectrum = 0,
	Spectrogram,
	Stereo,
	Loudness,
	Scope,
	Count
};

// what the spectrum plots. the stereo tab always measures the real left/right field
// regardless of this - it is only about which two traces the curve view draws
enum class AnalyzerChannelMode {
	LeftRight = 0,
	MidSide,
	Mono,
	Count
};

// how the oscilloscope picks the window it draws
enum class AnalyzerTriggerMode {
	Free = 0,   // whatever is newest in the ring, so the trace runs
	Rising,     // the last upward zero crossing, so a steady tone stands still
	Tempo,      // the last beat boundary, so a loop lines up with the grid
	Count
};

// ================================================================
// ANALYZER
// ================================================================

// a measurement device: drop it anywhere in a chain (or on the master) and it reports
// what is actually there. it is bit-transparent by construction - Process only ever
// reads the buffer, because a readout about a signal the next device does not receive
// is worse than no readout
//
// five tabs, all fed from one capture:
//   spectrum    - log frequency curve, left/right or mid/side, tilt and peak hold
//   spectrogram - the same spectrum scrolling over time, for spotting what only
//                 happens during the drop
//   stereo      - goniometer, overall correlation, and correlation/width per band,
//                 which is the readout that answers "is the sub mono and the top wide"
//   loudness    - momentary/short-term/integrated loudness and range per BS.1770-4,
//                 sample peak, true peak, RMS, crest factor and dc offset
//   scope       - the waveform itself, free, edge-triggered or locked to the beat
//
// THREADING. the split is deliberate and load-bearing:
//   audio thread - everything that integrates over time and would be wrong if a frame
//                  were missed: peaks, RMS, correlation, and the K-weighted sub-block
//                  sums behind every loudness figure. all O(n), all published through
//                  atomics or a single-producer ring
//   ui thread    - everything expensive and re-derivable: the transforms, the log fold,
//                  the loudness gating, all drawing. a torn read of the capture ring
//                  costs one frame of a slightly stale spectrum and nothing else
class AnalyzerProcessor : public AudioProcessor {
public:
	// ---- capture ----
	// the 4096-point window is 85 ms at 48 kHz: long enough that the low end has real
	// resolution, short enough that a transient still shows up while it is happening,
	// and the ring holds four windows so the scope can look further back than the spectrum
	static constexpr int kTransformSize = 4096;
	static constexpr int kRingSize = 16384; // power of two so the read index can mask

	// ---- display ----
	static constexpr int kSpectrumPoints = 240;
	static constexpr int kSpectrogramRows = 96;
	static constexpr int kSpectrogramColumns = 256;
	static constexpr int kStereoBands = 10;

	// ---- loudness ----
	// BS.1770-4 wants 400 ms blocks overlapped by 75%, so the accumulator closes every
	// 100 ms and a block is the mean of the last four. 3 s of them is a short-term window
	static constexpr int kSubBlocksPerShortTerm = 30;
	static constexpr int kSubBlocksPerMomentary = 4;
	static constexpr int kLoudnessRingSize = 512;

	// an hour of blocks at ten a second. past that the integrated readout stops
	// accumulating rather than growing without bound - nobody masters a single take
	// longer than that, and Reset clears it
	static constexpr int kMaxLoudnessBlocks = 36000;

	AnalyzerProcessor();
	~AnalyzerProcessor() override = default;

	const char* GetName() const override { return "Analyzer"; }
	std::string GetProcessorId() const override { return "Analyzer"; }
	bool IsInstrument() const override { return false; }

	void PrepareToPlay(double sampleRate) override;
	void Reset() override;

	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages,
				 const ProcessContext& context) override;

	bool RenderCustomUI(const ImVec2& size) override;

	// the view state is a set of switches, not values to automate, so it needs its own
	// lines beside the (empty) inherited parameter block
	void CopyStateFrom(const AudioProcessor& other) override;
	void Save(std::ostream& out) override;
	void Load(std::istream& in) override;

	// ================================================================
	// HEADLESS READOUT
	// ================================================================

	// every number the tabs draw is reachable from here, so the analysis can be driven
	// and asserted without an ImGui context. UI thread only, all of it

	// recompute the spectra, the stereo bands and the loudness gating from what the
	// audio thread has published. RenderCustomUI calls this once per frame
	void Refresh();

	// hand the spectrogram some wall clock; it appends a column at a fixed cadence so
	// the scroll speed does not follow the frame rate
	void AdvanceSpectrogram(float deltaSeconds);

	// clear the held peaks, the loudness history and the spectrogram, and leave the
	// live meters to refill. the audio thread picks the request up on its next block
	void ResetMeasurements();

	// the view switches, so the readout can be pointed at what a caller wants to
	// measure without going through the tab bar
	AnalyzerView GetView() const { return (AnalyzerView)mView; }
	void SetView(AnalyzerView view);
	AnalyzerChannelMode GetChannelMode() const { return (AnalyzerChannelMode)mChannelMode; }
	void SetChannelMode(AnalyzerChannelMode mode);

	// dB per octave, pivoting at 1 kHz. zero is the raw spectrum
	float GetTilt() const { return mTiltDbPerOctave; }
	void SetTilt(float dbPerOctave);

	int GetSpectrumPointCount() const { return kSpectrumPoints; }
	double GetSpectrumPointFrequency(int index) const;

	// trace 0 is left / mid / mono and trace 1 is right / side, following the channel
	// mode. dB is post-tilt, which is what the curve draws
	float GetSpectrumPointDb(int trace, int index) const;

	// channel 0 is left, 1 is right, everywhere below
	float GetPeakDb(int channel) const;
	float GetTruePeakDb(int channel) const;
	float GetRMSDb(int channel) const;
	float GetOffset(int channel) const; // mean sample value over ~1 s, the dc readout

	// peak over RMS across the whole capture ring - a third of a second. a healthy mix
	// sits around 10-20 dB; watching this collapse is the cheapest way to see a
	// compressor being overdone. recomputed by Refresh, like the spectra
	float GetCrestFactorDb() const { return mCrestFactorDb; }

	// +1 mono, 0 uncorrelated, -1 one channel against the other
	float GetCorrelation() const;

	// widthRatio is side energy over total: 0 dead center, 0.5 fully decorrelated, 1 an
	// inverted pair that disappears the moment anything sums it to mono
	void GetStereoBand(int index, double& lowFrequency, double& highFrequency,
					   float& correlation, float& widthRatio, float& db) const;

	// all four in LUFS / LU, -100 while there is not yet enough audio to say
	float GetMomentaryLoudness() const;
	float GetShortTermLoudness() const;
	float GetIntegratedLoudness() const { return mIntegratedLoudness; }
	float GetLoudnessRange() const { return mLoudnessRange; }
private:
	// one biquad and its direct form I memory. form I because the K-weighting shelf is
	// evaluated in double and its accuracy matters more than the extra state
	struct AnalyzerBiquad {
		double b0 = 1.0;
		double b1 = 0.0;
		double b2 = 0.0;
		double a1 = 0.0;
		double a2 = 0.0;
	};

	struct AnalyzerBiquadState {
		double x1 = 0.0;
		double x2 = 0.0;
		double y1 = 0.0;
		double y2 = 0.0;
	};

	// one finished 100 ms step, handed to the UI thread so the gating the standard
	// needs never runs on the audio thread
	struct LoudnessBlock {
		float momentaryPower = 0.0f; // mean weighted power of the last 400 ms
		float shortTermPower = 0.0f; // ... and of the last 3 s
		bool shortTermValid = false; // false until 3 s of audio has actually gone by
	};

	struct StereoBand {
		double lowFrequency = 0.0;
		double highFrequency = 0.0;
		float correlation = 0.0f;
		float widthRatio = 0.0f;
		float db = -120.0f;
	};

	// ================================================================
	// AUDIO THREAD
	// ================================================================

	void DesignWeighting(double rate);
	static double ApplyBiquad(const AnalyzerBiquad& coefficients, AnalyzerBiquadState& state, double x);

	// close the running 100 ms accumulator, republish momentary and short-term, and
	// hand the block to the UI thread
	void CloseSubBlock();

	// mean of the newest `count` sub-block powers, over however many exist so far
	double MeanSubBlockPower(int count) const;

	// 4x oversampled peak of one channel of the block, per BS.1770-4 annex 2. always
	// carries the block's tail forward into slot `slot` so the interpolation stays
	// continuous across the boundary; only actually interpolates when `measure` is set,
	// because inter-sample peaks are a question about signals near full scale and a
	// quiet block should not pay 48 taps a sample to be told it is quiet
	float MeasureTruePeak(const float* buffer, int numFrames, int numChannels,
						  int bufferChannel, int slot, bool measure);

	void ClearAudioState();

	// ================================================================
	// UI THREAD
	// ================================================================

	void ComputeSpectra();
	void ComputeStereoBands();
	void ComputeCrestFactor();
	void DrainLoudnessBlocks();
	void RecomputeIntegratedLoudness();
	void RecomputeLoudnessRange();

	// the dB the curve draws at `index`: the folded level plus the tilt, which pivots
	// at 1 kHz so turning it up does not move the middle of the graph
	float TiltedDb(float db, double frequency) const;

	// ---- ui pieces ----
	void DrawSpectrum(const ImVec2& pos, const ImVec2& size);
	void DrawSpectrogram(const ImVec2& pos, const ImVec2& size);
	void DrawStereo(const ImVec2& pos, const ImVec2& size);
	void DrawLoudness(const ImVec2& pos, const ImVec2& size);
	void DrawScope(const ImVec2& pos, const ImVec2& size);
	void DrawControlRow(float width);

	// the log frequency grid and its labels, shared by the spectrum and the spectrogram
	void DrawFrequencyGrid(const ImVec2& pos, const ImVec2& size, bool labels) const;

	// a labelled number with a bar under it, the unit of the loudness tab's layout
	void DrawReadout(const ImVec2& pos, const ImVec2& size, const char* label,
					 const char* value, float fill, unsigned int fillColor) const;

	// ================================================================
	// STATE
	// ================================================================

	double mSampleRate = 48000.0;

	// ---- capture ring: audio thread appends, ui thread transforms ----
	std::vector<float> mRingLeft;
	std::vector<float> mRingRight;
	std::atomic<int> mRingWrite{0};

	// project sample position of the newest sample in the ring, and the block's tempo,
	// so the scope can line its window up with the beat grid
	std::atomic<int64_t> mRingSample{0};
	std::atomic<double> mSamplesPerBeat{24000.0};
	std::atomic<bool> mPlaying{false};

	// ---- meters the audio thread publishes ----
	std::array<std::atomic<float>, 2> mPeak{};
	std::array<std::atomic<float>, 2> mHeldPeak{};
	std::array<std::atomic<float>, 2> mTruePeak{};
	std::array<std::atomic<float>, 2> mRMS{};
	std::array<std::atomic<float>, 2> mOffset{};
	std::atomic<float> mCorrelation{1.0f};
	std::atomic<float> mMomentaryLoudness{-100.0f};
	std::atomic<float> mShortTermLoudness{-100.0f};

	// set by the UI, consumed and cleared by the next block
	std::atomic<bool> mResetRequest{false};

	// ---- audio thread only ----
	double mMeterCoefficient = 0.0;	 // one-pole rate for the ~300 ms averages
	double mOffsetCoefficient = 0.0; // ... and the ~1 s dc mean
	std::array<double, 2> mRMSState{};
	std::array<double, 2> mOffsetState{};
	std::array<double, 2> mHeldPeakState{};
	double mCorrelationLeftRight = 0.0;
	double mCorrelationLeftLeft = 0.0;
	double mCorrelationRightRight = 0.0;

	AnalyzerBiquad mWeightShelf;
	AnalyzerBiquad mWeightHighPass;
	std::array<std::array<AnalyzerBiquadState, 2>, 2> mWeightState{}; // [stage][channel]

	int mSubBlockSamples = 4800;
	int mSubBlockFill = 0;
	double mSubBlockSum = 0.0;
	std::array<double, kSubBlocksPerShortTerm> mSubBlockPower{};
	int mSubBlockWrite = 0;
	int mSubBlockCount = 0;

	// the samples immediately before the current block, so the true peak interpolator
	// does not restart from silence on every boundary
	static constexpr int kTruePeakPhases = 4;
	static constexpr int kTruePeakTaps = 12; // per phase, so 48 in total
	std::array<std::array<float, kTruePeakTaps>, 2> mTruePeakTail{};

	// ---- loudness handoff ----
	std::array<LoudnessBlock, kLoudnessRingSize> mLoudnessRing{};
	std::atomic<int> mLoudnessWrite{0}; // monotonic, masked on access
	int mLoudnessRead = 0;				// ui thread only

	// ---- ui thread scratch ----
	// per-instance rather than function-local statics: the stereo bands are taken from
	// the same two complex spectra the curves are folded from, so both have to outlive
	// a single call
	std::array<std::vector<float>, 2> mScratchSamples;
	std::array<std::vector<double>, 2> mScratchReal;
	std::array<std::vector<double>, 2> mScratchImaginary;
	std::vector<double> mScratchMagnitude;
	std::vector<float> mScratchDb;

	// ---- ui thread results ----
	std::array<std::vector<float>, 2> mSpectrumDb;
	std::array<std::vector<float>, 2> mSpectrumPeakDb;
	std::array<StereoBand, kStereoBands> mStereoBands{};
	std::vector<float> mSpectrogram; // kSpectrogramColumns rows of kSpectrogramRows
	int mSpectrogramWrite = 0;
	float mSpectrogramClock = 0.0f;

	std::vector<float> mIntegratedPower; // 400 ms block powers awaiting the gate
	std::vector<float> mRangePower;		 // 3 s block powers, for the 10th-95th spread
	float mIntegratedLoudness = -100.0f;
	float mLoudnessRange = 0.0f;
	float mCrestFactorDb = 0.0f;

	// ---- view state, serialized ----
	int mView = (int)AnalyzerView::Spectrum;
	int mChannelMode = (int)AnalyzerChannelMode::LeftRight;
	int mTriggerMode = (int)AnalyzerTriggerMode::Free;
	float mTiltDbPerOctave = 4.5f;
	float mFloorDb = -96.0f;
	float mCeilingDb = 6.0f;
	// the scope's window length. a parameter rather than a bare float so it is edited
	// with the same value box as every other number in the app; it stays out of
	// mParameters because it moves nothing in the audio and so has no automation lane
	KnobParameter mScopeWindow{"Window", 20.0f, 1.0f, 200.0f, ImGuiKnobVariant_Milliseconds};
	bool mShowPeakHold = true;
	bool mFrozen = false;
};
