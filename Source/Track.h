#pragma once
#include <vector>
#include <memory>
#include <algorithm>
#include <string>
#include <atomic>
#include <iostream>
#include "AudioProcessor.h"
#include "MIDITypes.h"
#include "Clip.h"
#include "imgui.h" // for ImU32
#include "Parameter.h"

// automation structures
struct AutomationPoint {
	double beat;
	float value;		  // parameter value
	float tension = 0.0f; // -1.0 to 1.0 (0.0 is linear)
	bool selected = false;
};

struct AutomationCurve {
	Parameter* targetParam = nullptr;
	std::string paramName; // used for serialization restoration
	std::vector<AutomationPoint> points;

	// helper to get value at specific beat
	float Evaluate(double beat) const;
};

class Track {
public:
	Track();
	~Track();

	void SetName(const std::string& name) { mName = name; }
	const std::string& GetName() const { return mName; }

	// color (ImU32 - ABGR packed)
	void SetColor(ImU32 color) { mColor = color; }
	ImU32 GetColor() const { return mColor; }

	// mixer controls
	Parameter* GetVolumeParameter() { return mVolumeParam.get(); }
	Parameter* GetPanParameter() { return mPanParam.get(); }
	Parameter* GetBpmParameter() { return mBpmParam.get(); }

	void InitMasterTrackParameters(float initialBpm);

	void SetMute(bool mute) { mMute = mute; }
	bool GetMute() const { return mMute; }

	void SetSolo(bool solo) { mSolo = solo; }
	bool GetSolo() const { return mSolo; }

	// metering
	float GetPeakL() const { return mPeakL.load(); }
	float GetPeakR() const { return mPeakR.load(); }

	// initialize all processors in the chain
	void PrepareToPlay(double sampleRate);

	// reset all processors (silence audio)
	void Reset();

	// send note-offs to held instrument notes without disturbing effect DSP state, so it is
	// safe to call mid-playback (e.g. at a loop wrap) without cutting delay/reverb tails
	void AllNotesOff();

	// clear internal buffer (used for group mixing)
	void ClearAccumulator();
	// add audio from a child track into this track
	void AddToAccumulator(const float* input, int numFrames, int numChannels);

	// process the entire chain for this track
	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages,
				 const ProcessContext& context,
				 bool accumulateToOutput = false);

	// processor management
	void AddProcessor(std::shared_ptr<AudioProcessor> processor);
	void InsertProcessor(int index, std::shared_ptr<AudioProcessor> processor);
	void RemoveProcessor(int index);
	void MoveProcessor(int fromIndex, int toIndex);

	std::vector<std::shared_ptr<AudioProcessor>>& GetProcessors() { return mProcessors; }

	// clip management
	void AddClip(std::shared_ptr<Clip> clip);
	void RemoveClip(std::shared_ptr<Clip> clip);
	std::vector<std::shared_ptr<Clip>>& GetClips() { return mClips; }
	// undo support: replace the clip list wholesale, without overlap resolution
	void SetClips(std::vector<std::shared_ptr<Clip>> clips) { mClips = std::move(clips); }

	// trims or deletes clips that overlap with the activeClip
	void ResolveOverlaps(std::shared_ptr<Clip> activeClip);

	// automation
	std::vector<Parameter*> GetAllParameters(); // returns track params + processor params
	AutomationCurve* GetAutomationCurve(Parameter* param);
	void AddAutomationPoint(Parameter* param, double beat, float value);
	void RemoveAutomationPoint(Parameter* param, int index);
	void SortAutomationPoints(Parameter* param);

	// undo support: snapshot/restore a curve's points wholesale
	std::vector<AutomationPoint> GetAutomationPoints(Parameter* param);
	void SetAutomationPoints(Parameter* param, const std::vector<AutomationPoint>& points);
	Parameter* FindParameter(const std::string& name);
	void EvaluateAutomation(double currentBeat);

	bool HasInstrument() const;

	// grouping support
	void SetGroup(bool isGroup) { mIsGroup = isGroup; }
	bool IsGroup() const { return mIsGroup; }

	void SetParent(std::shared_ptr<Track> parent) { mParent = parent; }
	std::shared_ptr<Track> GetParent() const { return mParent.lock(); }

	bool mIsCollapsed = false;

	// ui state
	bool mShowAutomation = false;
	Parameter* mSelectedAutomationParam = nullptr;

	// serialization
	void Save(std::ostream& out, int trackIndex);
	void Load(std::istream& in);
	// fixup automation pointers after processors loaded
	void RebindAutomation();

	// temp storage for parent index during loading
	int mLoadedParentIndex = -1;
private:
	std::string mName = "Track";
	ImU32 mColor = IM_COL32(100, 100, 100, 255);

	std::vector<std::shared_ptr<AudioProcessor>> mProcessors;
	std::vector<std::shared_ptr<Clip>> mClips;

	// mixer state
	std::unique_ptr<Parameter> mVolumeParam; // dB
	std::unique_ptr<Parameter> mPanParam;	 // -1 to 1
	std::unique_ptr<Parameter> mBpmParam;	 // BPM (Master Track only)
	bool mMute = false;
	bool mSolo = false;

	// metering (atomic for thread safety)
	std::atomic<float> mPeakL{0.0f};
	std::atomic<float> mPeakR{0.0f};

	// grouping
	bool mIsGroup = false;
	std::weak_ptr<Track> mParent;
	std::vector<float> mInputAccumulator; // buffer for group inputs

	// automation data
	std::vector<AutomationCurve> mAutomationCurves;
};
