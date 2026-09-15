#pragma once
#include <array>
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
	bool timelineGridAuto = false;
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

	// bounces track `index` - its clips through its devices and its fader, and for a group
	// everything nested under it - to a wav in `directory`, then drops that on a fresh
	// track created directly after it (after the whole group, for a group). the source
	// track is left exactly as it was: this adds a rendered copy, it does not replace one
	//
	// `endBeat` <= `startBeat` means "whatever the track holds", measured over its subtree
	// plus a tail for devices still ringing; pass a range to bounce a time selection
	//
	// the render deliberately ignores solo anywhere in the project and the source track's
	// own mute, because asking to render a track is asking for its audio, not for the
	// silence the current mix happens to give it. mutes further down inside a group are
	// respected - those are part of how the group is arranged
	//
	// returns the new track, or null when there was nothing to render (bad index, an
	// empty subtree, a file that would not open); nothing is added in that case
	std::shared_ptr<Track> RenderTrackToNewTrack(int index, const std::string& directory,
												double startBeat = 0.0, double endBeat = 0.0);

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
	std::shared_ptr<Track> InsertNewTrack(int index, std::shared_ptr<Track> parent);

	// the sibling-after-the-whole-subtree placement CreateTrackAfter is named for.
	// callers hold mMutex
	std::shared_ptr<Track> CreateTrackAfterInternal(int index);

	// true when `track` is `root` or lives anywhere below it
	static bool IsInSubtree(const std::shared_ptr<Track>& track, const std::shared_ptr<Track>& root);

	// the last beat any clip under `root` reaches, 0 when there are none. a null root
	// measures the whole project, so a full export and a single-track bounce ask the
	// same question of the same code. callers hold mMutex
	double SubtreeEndBeat(const std::shared_ptr<Track>& root) const;

	// the offline pass both bounces take. `track` null renders the whole graph through
	// the master, exactly as an export does; non-null renders that track's subtree alone
	// and leaves the master out, because the bounce is going back into the same project
	// and would otherwise be mastered twice. callers hold mMutex
	bool RenderToWav(const std::string& path, const std::shared_ptr<Track>& track,
					 double startBeat, double endBeat, double sampleRate);

	// splits a block's live keyboard MIDI into the per-track lists below. a note-on goes
	// to the selected instrument track; the release that ends it goes back to whatever
	// track that was, because only the instrument actually holding a note can let go of
	// it. resolving a release against the *current* selection instead meant changing
	// tracks (or muting one) under a held key delivered the note-off somewhere else and
	// left the note sounding on an instrument nothing would ever address again
	void RouteLiveMIDI(const std::vector<MIDIMessage>& liveMIDIEvents);

	// the live MIDI routed to `trackId` this block, null when there is none
	const std::vector<MIDIMessage>* LiveMIDIFor(int trackId) const;

	// whether `track` or anything below it is owed live MIDI this block. a silenced
	// track is normally skipped outright, and a group with it
	bool SubtreeHasLiveMIDI(const std::shared_ptr<Track>& track) const;

	// rebuild the sharing between linked (non-unique) MIDI clips once every track is
	// in memory: each clip parsed its own copy of the notes, so clips that were saved
	// with the same SEQ id are handed one sequence again. same two-pass shape as the
	// PARENT_IDX hierarchy fixup right beside it
	void RelinkMIDIClips();

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

	// which track each live keyboard note is sounding on, by Track::GetId(), -1 when
	// nothing holds it. survives across blocks: a key can be held down for as long as
	// the user likes, and the selection can move while it is
	std::array<int, 128> mLiveNoteOwner;

	// this block's live MIDI split by destination track id. a member rather than a
	// local so the audio thread reuses the storage instead of allocating per block
	std::vector<std::pair<int, std::vector<MIDIMessage>>> mLiveMIDIByTrack;

	// core dsp processing
	void ProcessAudioGraph(float* destinationBuffer, int numFrames, int numChannels, const ProcessContext& context, const std::vector<MIDIMessage>& liveMIDIEvents, bool anySolo);

	// recursive track helper
	void ProcessTrackRecursively(std::shared_ptr<Track> track, float* accumulationBuffer, int numFrames, int numChannels, const ProcessContext& context, bool anySolo);

	// true when this track, or anything below it, feeds a sidechain detector. drives
	// both the render order (producers before consumers) and the rule that a muted or
	// solo-excluded source still has to be rendered so its detector keeps running
	bool SubtreeFeedsSidechain(const std::shared_ptr<Track>& track) const;

	// internal helper
	void PrepareToPlayInternal(double sampleRate);
	void SetBpmInternal(double bpm);

	std::mutex mMutex;
};
