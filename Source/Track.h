#pragma once
#include <bitset>
#include <vector>
#include <memory>
#include <algorithm>
#include <string>
#include <atomic>
#include <cstdint>
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

	// stable per-track identity, unique within a session and preserved by save/load
	// unlike the track index it survives reordering and grouping, so cross-track
	// references (the Auto Sidechain source) can be stored safely
	uint32_t GetId() const { return mId; }

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
	// detectorOnly marks a render whose output is thrown away - a muted or
	// solo-excluded track that some other track sidechains from. it still runs (and
	// still feeds the detector bus) but must not light up its meter as if audible
	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages,
				 const ProcessContext& context,
				 bool accumulateToOutput = false,
				 bool detectorOnly = false);

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

	// a group is a mixing container: it sums its children and carries devices and
	// automation of its own, but never holds clips. an open automation lane owns the
	// row for as long as it is up, so nothing can be dropped there either. every
	// place that puts a clip on a track (menus, paste, file drop, cross-track drag)
	// asks this first
	bool AcceptsClips() const { return !mIsGroup && !mShowAutomation; }

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
	uint32_t mId = 0;
	// no in-class default: Track::Track sets this from Theme::TrackColor, so a
	// literal here would be a second, dead source of truth for a color
	ImU32 mColor = 0;

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

	// note numbers this track's sequencer has sounded and not yet released.
	// the sequencer is otherwise stateless: every block it recomputes each note's on and
	// off sample from the clip data and fires whatever lands in the window. so anything
	// that changes that data between the two - dragging a note to another pitch, deleting
	// it, deactivating or moving its clip, a tempo change, an undo, the playhead leaving
	// the clip - orphans the note-on, and the instrument holds that key forever. this is
	// the record of what actually sounded, and Process reconciles the clip data against it
	// every block so an orphan is released on the very next one
	std::bitset<128> mSoundingNotes;

	// automation data
	std::vector<AutomationCurve> mAutomationCurves;
};
