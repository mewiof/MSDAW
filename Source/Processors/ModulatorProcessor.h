#pragma once
#include "AudioProcessor.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// FD: a target names a track and a device inside it, neither of which this header needs
class Project;
class Track;

// ================================================================
// MODULATOR
// ================================================================
// a modulation source shaped like a device. it sits in a chain, passes audio through
// untouched and never looks at the note list; its entire output is the value it writes
// into other devices' parameters - exactly what a rack macro does, except the value
// comes from a generator instead of from a knob somebody is holding
//
// three generators, one phase:
//   LFO       - a waveform per lane, read continuously
//   Performer - the cycle is cut into steps, and each step plays a curve shape at its
//               own height. drawing writes both at once - the height from where the
//               pointer is, the shape from the palette - because a brush that only
//               wrote half of a step made picking a curve look like it did nothing.
//               right-drag is the way to reshape an envelope without restyling it
//   Stepper   - the cycle is cut into steps, and each step holds a level
//
// NOTE: the Performer's heights and the Stepper's levels are separate patterns even
// though they are the same idea, so that flipping modes to audition one is free. sharing
// them meant a mode switch and a tweak silently overwrote the pattern you came from
//
// every mode has two lanes (A and B) blended by one XFade control, so the crossfader
// means the same thing everywhere and is itself an ordinary automatable Parameter
//
// NOTE: a modulator writes its targets during its own Process call, so devices sitting
// EARLIER in the chain than it do not see this block's value until the next one. put it
// first in the chain (or first in the rack chain) unless a one-block lag is wanted. the
// rack's macros have the same property for the same reason
//
// NOTE: modulation is destructive, again like a macro: the target parameter's value IS
// the modulated value, so the knob visibly moves and whatever it held before is gone.
// the mapping's min/max is what the user set, not an offset around it
class ModulatorProcessor : public AudioProcessor {
public:
	static constexpr int kMaxSteps = 16;
	static constexpr int kLanes = 2;

	enum Mode {
		ModeLFO = 0,
		ModePerformer = 1,
		ModeStepper = 2,
		kNumModes = 3
	};

	// LFO waveforms. Random is a sample-and-hold that re-rolls once per step, so the
	// Steps control means the same thing here as it does in the other two modes
	enum Shape {
		ShapeSine = 0,
		ShapeTriangle,
		ShapeSawDown,
		ShapeSawUp,
		ShapeSquare,
		ShapeRandom,
		kNumShapes
	};

	// the shape one Performer step plays over its own slice of the cycle
	enum StepCurve {
		CurveSilent = 0,
		CurveHold,
		CurveRampUp,
		CurveRampDown,
		CurveExpRise,
		CurveExpFall,
		CurveTriangle,
		CurveHump,
		CurvePulse,
		CurveDoublePulse,
		CurveStairUp,
		CurveStairDown,
		kNumCurves
	};

	// one parameter this modulator drives. the track and the device are held weakly and
	// the parameter is re-resolved by name rather than pointed at, so an undo that takes
	// a device out of the project and puts it back cannot leave the audio thread writing
	// into something that has left - the same contract a rack macro mapping has, widened
	// from "inside this rack" to "anywhere in the project"
	struct Target {
		uint32_t trackId = 0;
		std::weak_ptr<Track> track;
		std::weak_ptr<AudioProcessor> device; // expired/empty means the track's own fader
		std::string paramName;

		// what to print when the saved address no longer resolves, so a project whose
		// target track was deleted can still say what it used to drive
		std::string trackName;
		std::string deviceName;

		// min > max is a deliberate inversion, so the span is signed
		float minValue = 0.0f;
		float maxValue = 1.0f;
		bool enabled = true;

		Parameter* resolved = nullptr; // cache, rebuilt when the chain generation moves

		// a saved address names its device by a path into a track that has not been read
		// back in yet, so resolution is deferred to the first refresh after the load
		std::string pendingPath;
		bool pending = false;
	};

	// everything an edit outside the parameter list changes, snapshotted whole for the
	// undo history: the patterns the grid draws and the target list
	struct State {
		float levels[kLanes][kMaxSteps] = {};  // Stepper
		float heights[kLanes][kMaxSteps] = {}; // Performer
		int curves[kLanes][kMaxSteps] = {};
		std::vector<Target> targets;
	};

	ModulatorProcessor();
	~ModulatorProcessor() override;

	const char* GetName() const override;
	std::string GetProcessorId() const override { return "Modulator"; }
	bool IsInstrument() const override { return false; }

	void PrepareToPlay(double sampleRate) override;
	void Reset() override;

	// NOTE: never reads or writes `buffer`, and never touches `mIDIMessages` beyond
	// looking for a note-on to retrigger on
	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages,
				 const ProcessContext& context) override;

	bool RenderCustomUI(const ImVec2& size) override;

	void CopyStateFrom(const AudioProcessor& other) override;

	// the patterns and the target addresses are not parameter values, so they need
	// their own lines beside the inherited parameter block
	void Save(std::ostream& out) override;
	void Load(std::istream& in) override;

	// ---- targets ----
	const std::vector<Target>& GetTargets() const { return mTargets; }
	std::vector<Target>& GetTargetsMutable() { return mTargets; }

	// addresses `parameter` wherever it lives in `project` and starts driving it over
	// its own full range, which is where the reference product opens a fresh mapping.
	// re-adding the same parameter updates that entry instead of stacking a second.
	// returns false when the parameter belongs to nothing the project can name
	bool AddTarget(Project* project, const Parameter* parameter);
	void RemoveTarget(int index);
	// true when this modulator drives that parameter, and so it is no longer the user's
	// to turn
	//
	// NOTE: this re-resolves the bindings, which the audio thread also does. call it
	// from the UI thread only while the graph is quiet (a test), or accept the same
	// benign tear a live knob turn already has
	bool IsParameterTargeted(const Parameter* parameter);

	// ---- undo support ----
	State CaptureState() const;
	void ApplyState(const State& state);

	// the step grid and the randomizer are drawn by the device itself, from inside
	// RenderCustomUI, where there is no EditorContext to push an undo entry with. so a
	// finished pattern edit is parked here and collected by the view, which holds the
	// shared_ptr an undo action needs to keep the device alive
	bool TakePatternEdit(State& before, State& after, std::string& name);

	// ---- ui readouts ----
	float GetVisualValue() const { return mVisValue; }
	float GetVisualPhase() const { return mVisPhase; }

	int CurrentMode() const;
	int StepCount() const;
private:
	// ---- generator ----
	double CycleSeconds(const ProcessContext& context) const;
	float LaneValue(int lane, float phase);
	float EnvelopeValue() const;
	void Retrigger(int64_t currentSample);
	float NextRandom();

	// ---- bindings ----
	// re-resolves every target if any chain in the session has changed since the last
	// check. a plain load in the common case; the project walk only runs when something
	// structural actually moved
	void RefreshBindings();
	void ApplyTargets(float value);

	// ---- pattern edits ----
	void BeginPatternEdit();
	void EndPatternEdit(const char* name);

	// ---- ui ----
	void RenderHeaderRow();
	void RenderCurvePalette(const ImVec2& size);
	void RenderLanePanel(int lane, const ImVec2& size);
	void RenderKnobRow(float width, float height);
	void RandomizePattern();

	// ---- parameters ----
	Parameter* pMode = nullptr;
	Parameter* pSync = nullptr;
	Parameter* pRate = nullptr;	 // Hz, free-running
	Parameter* pDivision = nullptr;	 // index into the beat-division table, synced
	Parameter* pSteps = nullptr;
	Parameter* pShapeA = nullptr;
	Parameter* pShapeB = nullptr;
	Parameter* pXFade = nullptr;
	Parameter* pDepth = nullptr;
	Parameter* pOffset = nullptr;
	Parameter* pSmooth = nullptr; // ms, the Stepper's glide
	Parameter* pPhase = nullptr;  // degrees
	Parameter* pRestart = nullptr;
	Parameter* pAttack = nullptr;
	Parameter* pDecay = nullptr;
	Parameter* pEnvAmount = nullptr;

	// ---- pattern ----
	float mLevels[kLanes][kMaxSteps] = {};	// Stepper
	float mHeights[kLanes][kMaxSteps] = {}; // Performer, one per curve
	int mCurves[kLanes][kMaxSteps] = {};

	// the array the mode on screen is drawing and dragging
	float* StepValues(int lane) { return CurrentMode() == ModePerformer ? mHeights[lane] : mLevels[lane]; }

	std::vector<Target> mTargets;

	// ---- dsp state ----
	double mSampleRate = 48000.0;
	double mFreePhase = 0.0;	 // cycles, advanced by wall time when not chasing the transport
	int64_t mAnchorSample = 0;	 // where the current cycle's phase 0 sits, in Restart mode
	bool mAnchored = false;
	double mEnvPosSec = -1.0; // negative means idle
	float mSmoothed = 0.0f;
	bool mSmoothPrimed = false;
	float mRandomValue[kLanes] = {};
	int mRandomStep[kLanes] = {-1, -1};
	uint32_t mRandomState = 0x9e3779b9u;

	// ---- ui readouts, written by the audio thread ----
	// a torn read costs at most one wrong pixel for one frame, so these are plain floats
	// rather than anything the audio thread could block on
	float mVisValue = 0.0f;
	float mVisPhase = 0.0f;

	// ---- binding state ----
	uint32_t mBoundGeneration = 0;

	// ---- ui state (not serialized) ----
	int mBrush = CurveRampDown; // the curve drawing gives a Performer step
	bool mSnapToGrid = false;
	int mDragLane = -1; // the lane a drag started in, so it cannot jump lanes
	// a right-drag moves heights and leaves the curves alone. latched when the gesture
	// starts, so letting go of one button mid-drag cannot turn it into the other
	bool mHeightOnly = false;

	// ---- pattern edit gesture ----
	bool mPatternEditing = false;
	State mPatternBefore;
	bool mPatternEditReady = false;
	State mPatternAfter;
	std::string mPatternEditName;
};
