#pragma once
#include "AudioProcessor.h"
#include "Parameters/KnobParameter.h"
#include <cstdint>
#include <vector>

// ================================================================
// PHASER
// ================================================================
// a chain of second-order allpass sections, summed back with the dry signal. an allpass
// passes every frequency at full level and only turns its phase, so the chain on its own
// is inaudible - the notches appear in the SUM, wherever the phase has come round far
// enough for the two paths to cancel. each section turns the phase through a full 360
// degrees and so contributes exactly one notch, which is what Poles counts
//
// the panel, the control set and the value formats follow the reference product's (now
// legacy) Phaser one for one: a Poles / Earth-Space / Color / Dry-Wet column, an XY field
// over Frequency and Feedback, an envelope follower, and an LFO with a sample-and-hold
// shape and a Phase / Spin stereo pair
//
// NOTE: the structure and the numbers in PhaserProcessor.cpp are not guesses - they were
// measured by rendering the same audio through the reference product and through this one
// and fitting. what came back: Poles second-order sections sitting on ONE frequency (not
// spread), Color as their Q, dry summed with the chain at half, and a feedback loop whose
// gain runs to about 0.58 rather than to the 0.99 its readout shows. the fit residual over
// the whole file is 0.002 octaves of center frequency, so this is the actual topology
//
// Earth and Space are the one thing the reference leaves to the ear, and the split here is
// the reading its manual supports - the modes "change the spacing of the notches":
//   - Earth stacks every section on the same frequency, so the notches come out in the
//     octave-ish pattern one resonance produces, and Color sharpens or softens them. this
//     is the warm one, and the only mode Color reaches
//   - Space spreads the sections HARMONICALLY (f, 2f, 3f ...), so the notches come out
//     evenly spaced in Hz like a comb. this is the metallic one
class PhaserProcessor : public AudioProcessor {
public:
	// NOTE: Poles counts NOTCHES - one second-order allpass section each. that is what the
	// reference product's control does, measured off a render: at Poles 4 its response
	// carries four notches
	static constexpr int kMaxPoles = 12;

	// the span the frequency knob covers, and the range modulation is clamped back into
	static constexpr float kMinFrequency = 40.0f;
	static constexpr float kMaxFrequency = 15000.0f;

	// how the stages are spread around the frequency knob
	enum Spacing {
		SpacingEarth = 0,
		SpacingSpace = 1,
		kNumSpacings = 2
	};

	// LFO waveforms. Random is the sample-and-hold the section is named after: one fresh
	// value per cycle rather than a continuous curve
	enum Shape {
		ShapeSine = 0,
		ShapeTriangle,
		ShapeSaw,
		ShapeSquare,
		ShapeRandom,
		kNumShapes
	};

	// what the right channel's LFO does differently from the left one
	enum StereoMode {
		StereoPhase = 0, // same rate, offset from the left one by Phase degrees
		StereoSpin = 1,	 // detuned rate, so the two drift apart. free-running rates only
		kNumStereoModes = 2
	};

	PhaserProcessor();
	~PhaserProcessor() override = default;

	const char* GetName() const override { return "Phaser"; }
	std::string GetProcessorId() const override { return "Phaser"; }
	bool IsInstrument() const override { return false; }

	void PrepareToPlay(double sampleRate) override;
	void Reset() override;

	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages,
				 const ProcessContext& context) override;

	bool RenderCustomUI(const ImVec2& size) override;

	// ---- readouts, also the seam the tests drive ----

	// magnitude of the whole device at `count` frequencies, in dB, taken from the current
	// parameter values with the modulation sitting at rest. this is the closed loop -
	// feedback resonance and dry/wet included - so what a test measures here is the shape
	// the audio path actually produces
	void GetResponseDb(const double* frequencies, float* outDb, int count) const;

	// the section center frequencies that Poles and Earth / Space put around
	// `centerFrequency`. writes at most kMaxPoles entries, returns how many it wrote
	int GetSectionFrequencies(float centerFrequency, float* out) const;

	// how sharply each section turns its phase, which is how wide the notch it makes comes
	// out. this is what Color sets, and only in Earth
	float SectionQ() const;

	// what the Feedback control is worth as a loop gain, 0..kMaxLoopGain
	float LoopGain() const;

	// where the modulation has pushed each channel's center frequency, for the display
	float GetVisualFrequency(int channel) const { return mVisFrequency[channel & 1]; }
private:
	// ---- parameters ----
	// NOTE: the dials are held as KnobParameter so the panel can size them to the rack
	// strip; the choosers are plain Parameters, drawn as buttons and menus here and as
	// sliders in the fallback list
	KnobParameter* pPoles = nullptr;	 // second-order sections, and so notches, 1..kMaxPoles
	Parameter* pSpacing = nullptr;		 // Spacing
	KnobParameter* pColor = nullptr;	 // %, section Q, Earth only
	KnobParameter* pDryWet = nullptr;	 // %
	KnobParameter* pFrequency = nullptr; // Hz, where the stages sit
	KnobParameter* pFeedback = nullptr;	 // 0..0.99, resonance

	KnobParameter* pEnvAmount = nullptr;  // %, signed
	KnobParameter* pEnvAttack = nullptr;  // ms
	KnobParameter* pEnvRelease = nullptr; // ms

	KnobParameter* pLFOAmount = nullptr; // %
	Parameter* pLFOShape = nullptr;		 // Shape
	Parameter* pLFOSync = nullptr;		 // 0 free (Hz), 1 tempo-synced
	KnobParameter* pLFORate = nullptr;	 // Hz, free
	Parameter* pLFODivision = nullptr;	 // index into the beat-division table, synced
	Parameter* pLFOStereo = nullptr;	 // StereoMode
	KnobParameter* pLFOPhase = nullptr;	 // degrees, Phase mode
	KnobParameter* pLFOSpin = nullptr;	 // %, Spin mode

	// ---- dsp state ----
	// one allpass chain per channel. the coefficients are refreshed on a control block
	// rather than per sample, so a twelve-section chain costs twelve tan() calls every
	// third of a millisecond instead of twelve per sample
	//
	// a second-order allpass has its numerator coefficients reversed from its denominator,
	// so the whole section is two numbers: y = c2 x + c1 x' + x'' - c1 y' - c2 y''
	struct ChannelState {
		float c1[kMaxPoles] = {};
		float c2[kMaxPoles] = {};
		float x1[kMaxPoles] = {}, x2[kMaxPoles] = {};
		float y1[kMaxPoles] = {}, y2[kMaxPoles] = {};
		float feedback = 0.0f; // the chain's own last output, what feeds back in
	};
	std::vector<ChannelState> mChannels;

	double mSampleRate = 48000.0;
	float mEnvelope = 0.0f; // the follower's smoothed peak, 0..1

	// cycles since the LFO started, one per stereo lane. deliberately NOT wrapped into
	// 0..1: the sample-and-hold needs to know which cycle it is in to re-roll once per
	// cycle, and a wrapped phase cannot tell a new cycle from the one it just left
	double mLFOPhase[2] = {};
	float mRandomValue[2] = {};
	int64_t mRandomCycle[2] = {-1, -1};
	uint32_t mRandomState = 0x9e3779b9u;

	// ---- ui readouts, written by the audio thread ----
	// a torn read costs one wrong pixel for one frame, so these are plain floats rather
	// than anything the audio thread could block on
	float mVisFrequency[2] = {1050.0f, 1050.0f};
	float mVisEnvelope = 0.0f;

	// ---- ui state (not serialized) ----
	float mDragOldFrequency = 0.0f;
	float mDragOldFeedback = 0.0f;

	// ---- helpers ----
	int SectionCount() const;
	int CurrentShape() const;
	float ModulatedFrequency(float modulation) const;
	float NextRandom();
	void UpdateCoefficients(ChannelState& state, float centerFrequency);

	// ---- ui ----
	void RenderMainColumn(float width, float height, float radius);
	void RenderField(const ImVec2& position, const ImVec2& size);
	void RenderEnvelopeColumn(float width, float height, float radius);
	void RenderLFOColumn(float width, float knobWidth, float height, float radius);
};
