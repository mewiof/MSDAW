#pragma once
#include "AudioProcessor.h"
#include "Parameters/KnobParameter.h"
#include <array>
#include <atomic>
#include <vector>

// the eight shapes a band can take, in the order its type menu lists them. the
// value is serialized and automatable, so entries are never reordered
enum class EQFilterType {
	LowCut48 = 0, // four cascaded 12 dB/oct sections
	LowCut12,
	LowShelf,
	Bell,
	Notch,
	HighShelf,
	HighCut12,
	HighCut48,
	Count
};

// which filter set each channel runs through. Stereo uses set A everywhere; the
// other two split the signal and give each half its own eight bands
enum class EQChannelMode {
	Stereo = 0,
	LeftRight,
	MidSide,
	Count
};

// one normalized biquad section (a0 already divided out)
struct EQBiquad {
	double b0 = 1.0;
	double b1 = 0.0;
	double b2 = 0.0;
	double a1 = 0.0;
	double a2 = 0.0;
};

// direct form II transposed memory, one per section per channel
struct EQBiquadState {
	double z1 = 0.0;
	double z2 = 0.0;
};

// the 2x interpolator/decimator behind the oversampling switch, a 31-tap
// linear-phase halfband. every even-offset tap of a halfband is zero, so the
// polyphase split leaves one phase as a plain delay and only sixteen taps ever
// multiply - and those are symmetric, so it costs eight
//
// NOTE: up plus down is 15 samples of latency at the base rate and nothing in this
// DAW compensates plugin delay yet, which is why oversampling is off by default
struct EQHalfband {
	static constexpr int kTaps = 16;	// the non-zero (even index) half of the kernel
	static constexpr int kHistory = 16; // power of two so the read index can mask

	std::array<double, kHistory> filtered{}; // phase that runs the taps
	std::array<double, kHistory> delayed{};	 // phase that is a pure delay
	int writeIndex = 0;

	void Reset();

	// one base-rate sample in, two 2x-rate samples out
	void Up(double in, double& out0, double& out1);

	// two 2x-rate samples in, one base-rate sample out
	double Down(double in0, double in1);

	// the shared kernel, windowed-sinc, designed on first use
	static const double* Kernel();
};

// ================================================================
// EQ EIGHT
// ================================================================

// eight-band parametric EQ modeled on Ableton's EQ Eight: the same eight band
// shapes, the same globals (Adaptive Q, Scale, output Gain), the same Stereo /
// L-R / M-S channel modes with two independently editable filter sets, the same
// "solo the band you are dragging" audition monitor and the same optional 2x
// oversampling
//
// the editor mirrors the original as well - a log frequency graph with a spectrum
// analyzer behind it and eight numbered handles sitting on the curve, the edited
// band's Freq/Gain/Q down the left, the eight band slots along the bottom, the
// globals down the right
//
// NOTE: A and B are filter *sets*, not channels. Stereo runs set A on every
// channel, L-R runs A on the left and B on the right, M-S runs A on the mid and B
// on the side. Edit picks which set the graph and the knobs address, so every band
// parameter exists twice and both halves are always saved
class EQEightProcessor : public AudioProcessor {
public:
	static constexpr int kNumBands = 8;
	static constexpr int kNumSets = 2;	   // A and B; B is only reachable in L-R and M-S
	static constexpr int kMaxSections = 4; // a 48 dB/oct cut is four cascaded biquads

	EQEightProcessor();
	~EQEightProcessor() override = default;

	const char* GetName() const override { return "EQ Eight"; }
	std::string GetProcessorId() const override { return "EQEight"; }
	bool IsInstrument() const override { return false; }

	void PrepareToPlay(double sampleRate) override;
	void Reset() override;

	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages,
				 const ProcessContext& context) override;

	bool RenderCustomUI(const ImVec2& size) override;

	// oversampling, audition and the analyzer are switches, not values to automate,
	// so they need their own lines beside the inherited parameter block
	void CopyStateFrom(const AudioProcessor& other) override;
	void Save(std::ostream& out) override;
	void Load(std::istream& in) override;

	// ---- headless queries ----

	// magnitude in dB at each of `count` frequencies. bandIndex >= 0 answers for that
	// band alone; -1 sums the whole set and folds in the output gain, which is what
	// the graph's main curve draws. every band is designed once for the whole sweep,
	// so a curve costs one design pass per frame rather than one per pixel
	//
	// NOTE: this reads the parameters directly, not the block-rate smoothed values the
	// audio thread runs, so the curve tracks the mouse with no lag
	void GetResponseDb(int setIndex, int bandIndex, const double* frequencies,
					   float* outputDb, int count) const;

	// ---- analyzer readout ----

	// the display points drawn behind the curve: log-spaced center frequencies and the
	// level in dBFS at each. public so the mapping from the transform's linear bins
	// onto the log axis can be driven headlessly - it is the part that goes wrong
	int GetAnalyzerPointCount() const { return kAnalyzerBins; }
	void GetAnalyzerPoint(int index, double& frequency, float& db) const;

	// recompute the spectrum from what the audio thread has appended. UI thread only
	void RefreshAnalyzer();

	bool IsOversampling() const { return mOversample.load(std::memory_order_relaxed); }
	void SetOversampling(bool enabled) { mOversample.store(enabled, std::memory_order_relaxed); }

	EQChannelMode GetChannelMode() const;

	// how many filter sets the current mode actually exposes (1 in Stereo, else 2)
	int GetActiveSetCount() const { return GetChannelMode() == EQChannelMode::Stereo ? 1 : kNumSets; }
private:
	struct BandParams {
		// the three the knob column draws are held as knobs rather than as plain
		// parameters, because that column sizes their dials to the height it was given
		KnobParameter* pFrequency = nullptr;
		KnobParameter* pGain = nullptr;
		KnobParameter* pQ = nullptr;
		Parameter* pType = nullptr;
		Parameter* pActive = nullptr;
	};

	// what the audio thread actually runs: the designed cascade plus the smoothed
	// values it was designed for, so a block only redesigns what moved
	struct BandRuntime {
		std::array<EQBiquad, kMaxSections> sections;
		int numSections = 0;
		bool active = false;
		int type = -1;
		double frequency = 1000.0;
		double gain = 0.0;
		double q = 0.71;
		bool primed = false; // false until the first block snaps the smoothers to the parameters
	};

	// biquad memory follows the channel, not the set, so switching mode (which
	// re-points channels at other sets) has to clear it
	using BandStates = std::array<std::array<EQBiquadState, kMaxSections>, kNumBands>;

	// ---- parameters ----
	std::array<std::array<BandParams, kNumBands>, kNumSets> mBands;
	Parameter* pMode = nullptr;
	Parameter* pAdaptQ = nullptr;
	Parameter* pScale = nullptr;
	Parameter* pOutputGain = nullptr;

	// ---- dsp state ----
	double mSampleRate = 48000.0;
	double mProcessRate = 48000.0; // sampleRate, doubled while oversampling
	std::array<std::array<BandRuntime, kNumBands>, kNumSets> mRuntime;
	std::vector<BandStates> mStates;
	std::vector<std::array<EQBiquadState, 2>> mAuditionStates;
	std::vector<EQHalfband> mUpsamplers;
	std::vector<EQHalfband> mDownsamplers;
	EQBiquad mAuditionSection;
	int mActiveMode = -1;
	bool mOversampleActive = false;

	bool mForceRedesign = true;

	// ---- switches the audio thread reads ----
	std::atomic<bool> mOversample{false};
	std::atomic<bool> mAudition{false};
	std::atomic<int> mAuditionBand{0};
	std::atomic<int> mAuditionSet{0};

	// ---- analyzer ----
	// the audio thread appends a mono sum here and the UI thread transforms it. a
	// torn read costs one frame of a slightly stale spectrum, which is why the write
	// index is a plain relaxed atomic rather than anything the audio thread waits on
	//
	// the 4096-point window is 85 ms of audio: long enough that the low end has real
	// resolution (11.7 Hz per bin at 48 kHz, where the log axis stretches a handful of
	// bins across half the graph), short enough that a transient still shows up while
	// it is happening
	static constexpr int kRingSize = 8192;
	static constexpr int kTransformSize = 4096;
	static constexpr int kAnalyzerBins = 240;
	std::vector<float> mAnalyzerRing;
	std::atomic<int> mAnalyzerWrite{0};
	std::vector<float> mAnalyzerDb; // smoothed display magnitudes, UI thread only

	// ---- ui state ----
	int mSelectedBand = 0;
	int mEditSet = 0;
	bool mShowAnalyzer = true;
	float mGraphRangeDb = 15.0f;

	// the band a graph handle is currently being dragged by, and what its axes held
	// when the drag started - a drag moves more than one parameter and the edit-gesture
	// slot is global and fits one
	int mDraggingBand = -1;
	float mDragOldFrequency = 0.0f;
	float mDragOldGain = 0.0f;
	float mDragOldQ = 0.0f;

	// where the drag was anchored, and under which modifiers. every frame resolves the
	// value from the anchor rather than accumulating deltas into it: a value that hits
	// the end of its range cannot absorb the rest of the movement, so an accumulating
	// drag leaves the handle behind the cursor and then sets off again from the wrong
	// place the moment the cursor turns around
	ImVec2 mDragAnchorMouse{0.0f, 0.0f};
	float mDragAnchorFrequency = 0.0f;
	float mDragAnchorGain = 0.0f;
	float mDragAnchorQ = 0.0f;
	bool mDragFine = false;
	bool mDragAdjustingQ = false;

	// ---- helpers ----
	void EnsureChannelState(int numChannels);
	void ResetFilterState();

	// pulls the smoothers toward the parameters once per block and redesigns any band
	// whose shape actually moved
	void UpdateCoefficients(int numFrames);

	// the gain and Q a band actually gets once Scale and Adaptive Q have had their say
	void ApplyGlobalShaping(int type, double& gainDb, double& q) const;

	// designs one band's cascade at `rate` and returns how many sections it wrote
	static int BuildSections(int type, double frequency, double gainDb, double q,
							 double rate, EQBiquad* sections);

	// the bandpass the audition monitor listens through, from the edited band
	void UpdateAuditionSection();

	// runs every band of `setIndex` over one sample of `channel`
	double ProcessSample(int channel, int setIndex, double sample);

	// ProcessSample plus the 2x round trip when oversampling is on
	double RunChannel(int channel, int setIndex, double sample);

	void PushAnalyzerSample(float sample);

	// ---- ui pieces ----
	// (re)pin a handle drag to the cursor's current position and the band's current
	// values. called when the drag starts and whenever a modifier changes under it
	void AnchorHandleDrag(const BandParams& params);

	void DrawGraph(const ImVec2& pos, const ImVec2& size);
	void DrawBandStrip(float width);

	// the two full-height side columns. both are handed the height they have to fit in
	// rather than choosing one: the device rack is a fixed-height strip that never
	// scrolls, so the dials shrink and the globals rows close up instead of overflowing
	void DrawKnobColumn(float width, float height);
	void DrawGlobals(float width, float height);
};
