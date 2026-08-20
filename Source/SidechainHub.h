#pragma once
#include <cstdint>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>

class Project;

// the detector bus that lets one track's signal drive a processor sitting on another
// track (Auto Sidechain). tracks are addressed by their stable id, not by index, so a
// reference survives reordering, grouping and save/load
//
// it is a global singleton for the same reason Theme is: AudioProcessor has no
// EditorContext and no back-pointer to its track, so a plugin cannot otherwise reach
// the rest of the project. it also carries the live Project pointer so a device UI can
// populate a track picker
//
// flow, once per audio block:
//   1. Project::ProcessAudioGraph calls BeginBlock (stale slots decay to silence)
//   2. every subscribed track calls Publish at the end of Track::Process (post-fader)
//   3. consumers call Read
// project orders the graph so producers run before consumers; when that is impossible
// (a self-reference, or a cycle) Read simply returns the previous block's audio, which
// costs one block of detector latency and never deadlocks
class SidechainHub {
public:
	static SidechainHub& Instance();

	// there is exactly one live Project (owned by AudioEngine); it registers itself
	void SetProject(Project* project);
	Project* GetProject() const;

	// ---- subscription (UI thread) ----
	// slots are never erased once created: a Read pointer handed to the audio thread
	// must stay valid even if the UI drops the last subscriber mid-block. a slot is a
	// few KB and only exists for tracks actually used as a source
	void Subscribe(uint32_t trackId);
	void Unsubscribe(uint32_t trackId);

	// ---- audio thread ----
	bool HasSources() const;
	bool IsSource(uint32_t trackId) const;

	void BeginBlock(int numFrames);
	void Publish(uint32_t trackId, const float* interleaved, int numFrames, int numChannels);

	// mono detector signal for this block, or nullptr when nothing feeds this id
	const float* Read(uint32_t trackId) const;
private:
	SidechainHub() = default;
	SidechainHub(const SidechainHub&) = delete;
	SidechainHub& operator=(const SidechainHub&) = delete;

	struct Slot {
		int refCount = 0;
		bool published = false;
		std::vector<float> mono;
	};

	// held for the whole of every hub call. these are per-block operations (a handful
	// of locks per callback), never per-sample, and the audio callback already takes
	// Project's mutex for the entire block, so this adds no new class of stall
	mutable std::mutex mMutex;
	std::unordered_map<uint32_t, std::unique_ptr<Slot>> mSlots;
	int mLiveSources = 0;
	int mBlockFrames = 0;
	Project* mProject = nullptr;
};
