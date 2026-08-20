#pragma once
#include "AudioProcessor.h"
#include <atomic>
#include <cstdint>
#include <vector>

// ducks the track it sits on from another track's signal - the classic "kick pushes
// the bass out of the way" move, without routing a physical send anywhere.
//
// the detector signal arrives through SidechainHub: the user picks a source track, the
// hub subscribes to it, and Project renders that track before this one so the reduction
// is computed from the current block
//
// three ways to turn that signal into gain reduction:
//   follow - a real sidechain compressor. threshold/ratio/knee with attack, hold and
//            release ballistics. tracks the source's dynamics.
//   duck   - the source only says *when*. every transient above the threshold fires a
//            fixed, hand-shaped ducking envelope, so every pump is identical no matter
//            how the kick was mixed. this is the "auto" behaviour
//   free   - no detector at all: the same envelope, retriggered by the transport on a
//            beat division. works with no source track selected
class AutoSidechainProcessor : public AudioProcessor {
public:
	AutoSidechainProcessor();
	~AutoSidechainProcessor() override;

	const char* GetName() const override { return "Auto Sidechain"; }
	std::string GetProcessorId() const override { return "AutoSidechain"; }
	bool IsInstrument() const override { return false; }

	void PrepareToPlay(double sampleRate) override;
	void Reset() override;

	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages,
				 const ProcessContext& context) override;

	bool RenderCustomUI(const ImVec2& size) override;

	// a duplicated / pasted device has to keep pointing at the same source track
	void CopyStateFrom(const AudioProcessor& other) override;

	// the source reference is a track id, not a value to automate, so it needs its own
	// lines in the project file alongside the inherited parameter block
	void Save(std::ostream& out) override;
	void Load(std::istream& in) override;

	uint32_t GetSourceTrackId() const { return mSourceTrackId; }
	void SetSourceTrackId(uint32_t trackId);
private:
	enum Mode {
		ModeFollow = 0,
		ModeDuck = 1,
		ModeFree = 2
	};

	// ---- parameters ----
	Parameter* pMode = nullptr;
	Parameter* pAmount = nullptr;	 // wet blend of the computed reduction, %
	Parameter* pThreshold = nullptr; // dB, Follow + Duck
	Parameter* pRatio = nullptr;	 // Follow
	Parameter* pKnee = nullptr;		 // dB, Follow
	Parameter* pDepth = nullptr;	 // dB (negative), Duck + Free
	Parameter* pAttack = nullptr;	 // ms
	Parameter* pHold = nullptr;		 // ms
	Parameter* pRelease = nullptr;	 // ms
	Parameter* pCurve = nullptr;	 // -1..1 envelope tension, Duck + Free
	Parameter* pRate = nullptr;		 // beat division index, Free
	Parameter* pHighPass = nullptr;	 // detector filters
	Parameter* pLowPass = nullptr;

	// ---- routing ----
	uint32_t mSourceTrackId = 0;
	std::string mSourceTrackName; // shown when the saved id no longer resolves
	bool mAutoPickAttempted = false;

	// ---- dsp state ----
	double mSampleRate = 48000.0;
	float mDetHpState = 0.0f;
	float mDetLpState = 0.0f;
	float mDetEnv = 0.0f;
	float mGrDb = 0.0f; // smoothed reduction, Follow
	int mHoldCounter = 0;
	bool mTriggerArmed = true;
	int mRearmCounter = 0;
	bool mEnvActive = false;
	double mEnvPosSec = 0.0;

	int CurrentMode() const;
	float EnvelopeAmount(double tSec) const; // 0 (open) .. 1 (fully ducked)
	double EnvelopeLengthSec() const;
	double FreeCycleBeats() const;

	// ---- ui readouts, written by the audio thread ----
	// a torn read here costs at most one wrong pixel for one frame, so these are plain
	// relaxed atomics rather than anything the audio thread could block on
	struct ScopePoint {
		float detDb = -90.0f;
		float grDb = 0.0f;
	};
	static const int kScopePoints = 480;
	std::vector<ScopePoint> mScope;
	std::atomic<int> mScopeWrite{0};
	int mScopeStride = 200;
	int mScopeCounter = 0;
	float mScopeDetPeak = 0.0f;
	float mScopeGrPeak = 0.0f;
	std::atomic<float> mVisGrDb{0.0f};
	std::atomic<float> mVisDetDb{-90.0f};
	std::atomic<float> mVisEnvPhase{-1.0f}; // -1 when the envelope is idle

	void PushScopePoint(float detDb, float grDb);

	// ---- ui state (not serialized) ----
	bool mListen = false; // monitor the detector instead of the track
	bool mShowScope = false;
	float mDragAxisSec = 0.0f; // time axis frozen for the duration of a handle drag
	bool mDraggingHandle = false;
};
