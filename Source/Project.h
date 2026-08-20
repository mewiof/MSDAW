#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include "Track.h"
#include "Transport.h"

struct ProjectViewState {
	float pixelsPerBeat = 60.0f;
	double selectionStart = 0.0;
	double selectionEnd = 0.0;
	float scrollX = 0.0f;
	float scrollY = 0.0f;
	int timelineGridNumerator = 1;
	int timelineGridDenominator = 4;
};

class Project {
public:
	Project();
	~Project();

	void Initialize();

	Transport& GetTransport() { return mTransport; }
	std::vector<std::shared_ptr<Track>>& GetTracks() { return mTracks; }
	std::shared_ptr<Track> GetMasterTrack() { return mMasterTrack; }

	// track management
	void CreateTrack();

	// creates a track directly below `index` and everything nested under it, as a
	// sibling of that track - so "after this one" on a group header lands after the
	// whole group rather than as its first child. an out-of-range index appends at the
	// root, which is what CreateTrack does
	void CreateTrackAfter(int index);
	void RemoveTrack(int index);
	void MoveTrack(int srcIndex, int dstIndex, bool asChild);

	// grouping
	void GroupSelectedTracks(const std::set<int>& indices);
	void UngroupTrack(int trackIndex);
	void CheckEmptyGroups();

	// selection
	void SetSelectedTrack(int index);

	// undo support: replace the whole track list (order + membership) atomically
	// parent relationships live on the tracks themselves (SetParent), so the
	// caller restores those before/after as needed
	void RestoreTracks(std::vector<std::shared_ptr<Track>> tracks);

	void PrepareToPlay(double sampleRate);

	// set bpm
	void SetBpm(double bpm);

	// audio callback
	void ProcessBlock(float* outputBuffer, int numFrames, int numChannels, std::vector<MIDIMessage>& liveMIDIEvents);

	// wav export
	bool RenderAudio(const std::string& path, double startBeat, double endBeat, double sampleRate = 48000.0);

	std::mutex& GetMutex() { return mMutex; }

	// serialization
	void Save(const std::string& path);
	void Load(const std::string& path);

	// view state
	ProjectViewState& GetViewState() { return mViewState; }
	void SetViewState(const ProjectViewState& state) { mViewState = state; }
private:
	// the one place a track is born, shared by both CreateTrack paths. callers hold
	// mMutex; a null parent means the root level
	void InsertNewTrack(int index, std::shared_ptr<Track> parent);

	Transport mTransport;
	std::vector<std::shared_ptr<Track>> mTracks;
	ProjectViewState mViewState;
	std::shared_ptr<Track> mMasterTrack;

	std::vector<float> mMixBuffer;
	bool mWasPlaying = false;
	// playhead position at the end of the previous processed block, used to
	// detect a discontinuous seek so we can flush stuck notes (-1 = no prior block)
	int64_t mLastBlockEndSample = -1;
	int mSelectedTrackIndex = 0;

	// core dsp processing
	void ProcessAudioGraph(float* destinationBuffer, int numFrames, int numChannels, const ProcessContext& context, const std::vector<MIDIMessage>& liveMIDIEvents, bool anySolo);

	// recursive track helper
	void ProcessTrackRecursively(std::shared_ptr<Track> track, float* accumulationBuffer, int numFrames, int numChannels, const ProcessContext& context, const std::vector<MIDIMessage>& liveMIDIEvents, bool anySolo);

	// true when this track, or anything below it, feeds a sidechain detector. drives
	// both the render order (producers before consumers) and the rule that a muted or
	// solo-excluded source still has to be rendered so its detector keeps running
	bool SubtreeFeedsSidechain(const std::shared_ptr<Track>& track) const;

	// internal helper
	void PrepareToPlayInternal(double sampleRate);
	void SetBpmInternal(double bpm);

	std::mutex mMutex;
};
