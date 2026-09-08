#include "PrecompHeader.h"
#include "Project.h"
#include "PathText.h"
#include "SidechainHub.h"
#include "Clips/MIDIClip.h"
#include "Clips/AudioClip.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <functional>
#include <unordered_map>

// version history
// 1: initial format
// 2: per-clip ENABLED flag; a clip saved without one loads as active
// 3: per-clip SEQ id on MIDI clips, so linked (non-unique) clips reload still linked.
//    a clip saved without one comes back unique, which is how it already behaved
// 4: per-clip REVERSED flag on audio clips. the file on disk is always the forward one,
//    so a clip saved without the flag loads forwards, which is how it already played
// 5: the Rack device, which nests chains of further devices under a PROCESSOR block:
//    RACK_* lines, a CHAIN_BEGIN block per chain, and MMAP lines naming a macro's
//    target by its path inside the rack. older projects have no racks and are unchanged
// 6: the Modulator device, which writes MOD_LEVELS / MOD_HEIGHTS / MOD_CURVES pattern
//    lines and a MOD_TARGET line per driven parameter, naming it by track id plus a
//    device path into that track. older projects have no modulators and are unchanged
const int kCurrentProjectVersion = 6;

Project::Project() {
	// device UIs reach the track list through the hub (an AudioProcessor has no
	// context pointer of its own); AudioEngine owns the one and only Project
	SidechainHub::Instance().SetProject(this);
}

Project::~Project() {
	if (SidechainHub::Instance().GetProject() == this)
		SidechainHub::Instance().SetProject(nullptr);
}

void Project::Initialize() {
	std::lock_guard<std::mutex> lock(mMutex);
	mTracks.clear();

	mMasterTrack = std::make_shared<Track>();
	mMasterTrack->SetName("Master");
	mMasterTrack->InitMasterTrackParameters(mTransport.GetBpm());
}

void Project::CreateTrack() {
	std::lock_guard<std::mutex> lock(mMutex);
	InsertNewTrack((int)mTracks.size(), nullptr);
}

void Project::CreateTrackAfter(int index) {
	std::lock_guard<std::mutex> lock(mMutex);
	CreateTrackAfterInternal(index);
}

std::shared_ptr<Track> Project::CreateTrackAfterInternal(int index) {
	if (index < 0 || index >= (int)mTracks.size())
		return InsertNewTrack((int)mTracks.size(), nullptr);

	// mTracks is flat and a group's children follow their header, so stepping over
	// every descendant is what puts the new track after the group instead of inside it
	const std::shared_ptr<Track> anchor = mTracks[index];
	int insertAt = index + 1;
	while (insertAt < (int)mTracks.size()) {
		if (!IsInSubtree(mTracks[insertAt], anchor))
			break;
		++insertAt;
	}

	// a sibling, so a track added below one that lives in a group joins that group
	return InsertNewTrack(insertAt, anchor->GetParent());
}

bool Project::IsInSubtree(const std::shared_ptr<Track>& track, const std::shared_ptr<Track>& root) {
	for (auto t = track; t; t = t->GetParent()) {
		if (t == root)
			return true;
	}
	return false;
}

// NOTE: callers hold mMutex. the audio thread walks mTracks for the whole block, so
// growing it anywhere but under that lock would pull the vector out from under it
std::shared_ptr<Track> Project::InsertNewTrack(int index, std::shared_ptr<Track> parent) {
	auto track = std::make_shared<Track>();
	track->SetName("Track " + std::to_string(mTracks.size() + 1));
	track->SetParent(parent);

	if (mTransport.GetSampleRate() > 0) {
		track->PrepareToPlay(mTransport.GetSampleRate());
	}
	mTracks.insert(mTracks.begin() + std::clamp(index, 0, (int)mTracks.size()), track);
	// a track brings a whole device chain with it. a modulator addresses parameters
	// across the project, so "which devices exist" changing here has to reach it the
	// same way a device added to one chain does
	ProcessorHost::BumpChainGeneration();
	return track;
}

void Project::RemoveTrack(int index) {
	std::lock_guard<std::mutex> lock(mMutex);
	if (index >= 0 && index < (int)mTracks.size()) {
		auto target = mTracks[index];
		if (target->IsGroup()) {
			for (auto& t : mTracks) {
				if (t->GetParent() == target) {
					t->SetParent(nullptr);
				}
			}
		}
		mTracks.erase(mTracks.begin() + index);
		ProcessorHost::BumpChainGeneration();
	}
}

void Project::MoveTrack(int srcIndex, int dstIndex, bool asChild) {
	std::lock_guard<std::mutex> lock(mMutex);
	if (srcIndex < 0 || srcIndex >= (int)mTracks.size())
		return;

	auto srcTrack = mTracks[srcIndex];
	mTracks.erase(mTracks.begin() + srcIndex);

	// if removed a track before destination -> shift destination index back
	if (dstIndex > srcIndex)
		dstIndex--;

	if (asChild) {
		if (dstIndex >= 0 && dstIndex < (int)mTracks.size()) {
			auto parent = mTracks[dstIndex];
			if (parent != srcTrack) {
				if (!parent->IsGroup()) {
					parent->SetGroup(true);
				}
				srcTrack->SetParent(parent);
				// insert after the group header
				mTracks.insert(mTracks.begin() + dstIndex + 1, srcTrack);
			} else {
				// fallback
				mTracks.insert(mTracks.begin() + dstIndex, srcTrack);
			}
		} else {
			mTracks.push_back(srcTrack);
			srcTrack->SetParent(nullptr);
		}
	} else {
		// normal reordering (gap drop)

		// sanity
		if (dstIndex < 0)
			dstIndex = 0;
		if (dstIndex > (int)mTracks.size())
			dstIndex = (int)mTracks.size();

		// instead of clearing the parent,
		// we check the track currently residing at 'dstIndex'
		// since we inserting before it, we should share its parent

		if (dstIndex < (int)mTracks.size()) {
			// inserting before an existing track
			auto neighbor = mTracks[dstIndex];
			srcTrack->SetParent(neighbor->GetParent());
		} else {
			// appending to the very end of the list
			// standard behavior is to place it at the root level
			srcTrack->SetParent(nullptr);
		}

		mTracks.insert(mTracks.begin() + dstIndex, srcTrack);
	}
	CheckEmptyGroups();
}

void Project::GroupSelectedTracks(const std::set<int>& indices) {
	if (indices.empty())
		return;
	std::lock_guard<std::mutex> lock(mMutex);

	std::vector<std::shared_ptr<Track>> targets;
	int minIndex = 999999;

	// 1. identify insertion point & collect targets
	for (int idx : indices) {
		if (idx >= 0 && idx < (int)mTracks.size()) {
			targets.push_back(mTracks[idx]);
			if (idx < minIndex)
				minIndex = idx;
		}
	}

	if (targets.empty())
		return;

	// 2. determine common parent logic for nested grouping
	// if all selected tracks share the same parent, the new group becomes a child of that
	std::shared_ptr<Track> commonParent = nullptr;
	bool firstCheck = true;
	bool allShareParent = true;

	for (auto& t : targets) {
		if (firstCheck) {
			commonParent = t->GetParent();
			firstCheck = false;
		} else {
			if (t->GetParent() != commonParent) {
				allShareParent = false;
				break;
			}
		}
	}

	if (!allShareParent) {
		commonParent = nullptr; // fallback to root if selection spans different hierarchy levels
	}

	// 3. create group
	auto groupTrack = std::make_shared<Track>();
	groupTrack->SetName("Group");
	groupTrack->SetGroup(true);
	if (commonParent)
		groupTrack->SetParent(commonParent);

	if (mTransport.GetSampleRate() > 0)
		groupTrack->PrepareToPlay(mTransport.GetSampleRate());

	// 4. remove from list
	// iterate reverse to preserve indices
	for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
		mTracks.erase(mTracks.begin() + *it);
	}

	// 5. insert group
	if (minIndex > (int)mTracks.size())
		minIndex = (int)mTracks.size();
	mTracks.insert(mTracks.begin() + minIndex, groupTrack);

	// 6. insert targets back as children
	int insertPos = minIndex + 1;
	for (auto& t : targets) {
		t->SetParent(groupTrack);
		mTracks.insert(mTracks.begin() + insertPos, t);
		insertPos++;
	}
}

void Project::UngroupTrack(int trackIndex) {
	std::lock_guard<std::mutex> lock(mMutex);
	if (trackIndex < 0 || trackIndex >= (int)mTracks.size())
		return;

	auto track = mTracks[trackIndex];
	if (!track->IsGroup())
		return;

	// parent of the group being fucked
	auto grandParent = track->GetParent();

	for (auto& t : mTracks) {
		if (t->GetParent() == track) {
			t->SetParent(grandParent); // promote to grandparent (or root)
		}
	}
	mTracks.erase(mTracks.begin() + trackIndex);
}

void Project::CheckEmptyGroups() {
	std::vector<std::shared_ptr<Track>> emptyGroups;
	for (auto& t : mTracks) {
		if (t->IsGroup()) {
			bool hasChild = false;
			for (auto& child : mTracks) {
				if (child->GetParent() == t) {
					hasChild = true;
					break;
				}
			}
			if (!hasChild)
				emptyGroups.push_back(t);
		}
	}
	for (auto& dead : emptyGroups) {
		auto it = std::find(mTracks.begin(), mTracks.end(), dead);
		if (it != mTracks.end())
			mTracks.erase(it);
	}
}

void Project::SetSelectedTrack(int index) {
	std::lock_guard<std::mutex> lock(mMutex);
	mSelectedTrackIndex = index;
}

void Project::RestoreTracks(std::vector<std::shared_ptr<Track>> tracks) {
	std::lock_guard<std::mutex> lock(mMutex);
	mTracks = std::move(tracks);
	// an undo can put a whole track back, devices and all
	ProcessorHost::BumpChainGeneration();
}

void Project::PrepareToPlayInternal(double sampleRate) {
	mTransport.SetSampleRate(sampleRate);
	for (auto& track : mTracks) {
		track->PrepareToPlay(sampleRate);
	}
	if (mMasterTrack)
		mMasterTrack->PrepareToPlay(sampleRate);
	mWasPlaying = false;
}

void Project::PrepareToPlay(double sampleRate) {
	std::lock_guard<std::mutex> lock(mMutex);
	PrepareToPlayInternal(sampleRate);
}

void Project::SetBpmInternal(double bpm) {
	double oldBpm = mTransport.GetBpm();
	double sampleRate = mTransport.GetSampleRate();

	// 1. convert current position (samples) to beats using old bpm
	// the cursor must stay at the same musical position
	int64_t currentPos = mTransport.GetPosition();
	double currentBeat = 0.0;

	// convert loop points so the loop region doesn't shift
	int64_t loopStart = mTransport.GetLoopStart();
	int64_t loopEnd = mTransport.GetLoopEnd();
	double loopStartBeat = 0.0;
	double loopEndBeat = 0.0;

	if (sampleRate > 0.0 && oldBpm > 0.0) {
		double oldBeatsPerSecond = oldBpm / 60.0;
		currentBeat = ((double)currentPos / sampleRate) * oldBeatsPerSecond;
		loopStartBeat = ((double)loopStart / sampleRate) * oldBeatsPerSecond;
		loopEndBeat = ((double)loopEnd / sampleRate) * oldBeatsPerSecond;
	}

	// 2. update transport bpm
	mTransport.SetBpm(bpm);

	if (mMasterTrack) {
		if (auto bpmParam = mMasterTrack->GetBpmParameter()) {
			bpmParam->value = bpm;
		}
	}

	// 3. convert beats back to samples using new bpm
	if (sampleRate > 0.0 && bpm > 0.0) {
		double newSecondsPerBeat = 60.0 / bpm;
		int64_t newPos = (int64_t)std::round(currentBeat * newSecondsPerBeat * sampleRate);
		mTransport.SetPosition(newPos);

		int64_t newLoopStart = (int64_t)std::round(loopStartBeat * newSecondsPerBeat * sampleRate);
		int64_t newLoopEnd = (int64_t)std::round(loopEndBeat * newSecondsPerBeat * sampleRate);
		mTransport.SetLoopRange(newLoopStart, newLoopEnd);
	}

	// 4. re-read every audio clip's length on the grid at the new tempo
	for (auto& track : mTracks) {
		for (auto& clip : track->GetClips()) {
			if (auto ac = std::dynamic_pointer_cast<AudioClip>(clip)) {
				ac->RetimeForBpmChange(oldBpm, bpm);
			}
		}
	}
}

void Project::SetBpm(double bpm) {
	std::lock_guard<std::mutex> lock(mMutex);
	SetBpmInternal(bpm);
}

void Project::ProcessTrackRecursively(std::shared_ptr<Track> track, float* destinationBuffer, int numFrames, int numChannels, const ProcessContext& context, const std::vector<MIDIMessage>& liveMIDIEvents, bool anySolo) {

	// determine if this track is "effectively soloed"
	// includes explicit/inheriting solo from an ancestor
	bool ancestorSolo = false;
	auto p = track->GetParent();
	while (p) {
		if (p->GetSolo()) {
			ancestorSolo = true;
			break;
		}
		p = p->GetParent();
	}

	bool isEffectiveSolo = track->GetSolo() || ancestorSolo;

	// a silenced track is normally skipped outright. the exception is a sidechain
	// source: it is still rendered, into a buffer we throw away, so muting the kick
	// (or soloing the bass to audition the ducking) does not stall the detector
	bool silenced = false;

	// mute is bypassed only by this track's OWN solo. an ancestor's solo says
	// "play this branch instead of the rest of the mix", not "unmute everything
	// inside it" - soloing a group must still respect a child the user muted
	if (track->GetMute() && !track->GetSolo())
		silenced = true;

	// global check
	if (!silenced && anySolo && !isEffectiveSolo) {
		bool childSolo = false;
		if (track->IsGroup()) {
			// recursively check if any descendant is soloed
			// keep the group chain active so the signal from the deep child can bubble up
			std::function<bool(std::shared_ptr<Track>)> hasSoloChild = [&](std::shared_ptr<Track> parent) -> bool {
				for (const auto& t : mTracks)
					if (t->GetParent() == parent) {
						if (t->GetSolo())
							return true;
						if (t->IsGroup() && hasSoloChild(t))
							return true;
					}
				return false;
			};
			childSolo = hasSoloChild(track);
		}
		if (!childSolo)
			silenced = true;
	}

	if (silenced && !SubtreeFeedsSidechain(track))
		return;

	if (track->IsGroup()) {
		track->ClearAccumulator();

		std::vector<std::shared_ptr<Track>> children;
		for (auto& child : mTracks) {
			if (child->GetParent() == track)
				children.push_back(child);
		}
		// same producer-before-consumer ordering as the root pass below
		if (SidechainHub::Instance().HasSources()) {
			std::stable_partition(children.begin(), children.end(),
								  [this](const std::shared_ptr<Track>& t) { return SubtreeFeedsSidechain(t); });
		}

		for (auto& child : children) {
			std::vector<float> childBuffer(numFrames * numChannels, 0.0f);
			ProcessTrackRecursively(child, childBuffer.data(), numFrames, numChannels, context, liveMIDIEvents, anySolo);
			// a silenced child leaves its buffer untouched, so this stays a no-op for it
			track->AddToAccumulator(childBuffer.data(), numFrames, numChannels);
		}
	}

	std::vector<float> processBuffer(numFrames * numChannels, 0.0f);
	std::vector<MIDIMessage> trackMIDI;
	int trackIdx = -1;
	for (int i = 0; i < (int)mTracks.size(); ++i)
		if (mTracks[i] == track)
			trackIdx = i;

	if (trackIdx == mSelectedTrackIndex && track->HasInstrument()) {
		trackMIDI = liveMIDIEvents;
	}

	// Track::Process publishes to the detector bus on its way out, which is the whole
	// point of having rendered a silenced source; only the audible sum is dropped
	track->Process(processBuffer.data(), numFrames, numChannels, trackMIDI, context, false, silenced);

	if (silenced)
		return;

	for (int i = 0; i < numFrames * numChannels; ++i) {
		destinationBuffer[i] += processBuffer[i];
	}
}

bool Project::SubtreeFeedsSidechain(const std::shared_ptr<Track>& track) const {
	if (!track)
		return false;
	const SidechainHub& hub = SidechainHub::Instance();
	if (hub.IsSource(track->GetId()))
		return true;
	if (!track->IsGroup())
		return false;
	for (const auto& child : mTracks) {
		if (child->GetParent() == track && SubtreeFeedsSidechain(child))
			return true;
	}
	return false;
}

void Project::ProcessAudioGraph(float* destinationBuffer, int numFrames, int numChannels, const ProcessContext& context, const std::vector<MIDIMessage>& liveMIDIEvents, bool anySolo) {
	if (mMixBuffer.size() < (size_t)(numFrames * numChannels)) {
		mMixBuffer.resize(numFrames * numChannels);
	}
	std::fill(mMixBuffer.begin(), mMixBuffer.begin() + (numFrames * numChannels), 0.0f);

	SidechainHub& hub = SidechainHub::Instance();
	hub.BeginBlock(numFrames);

	std::vector<std::shared_ptr<Track>> roots;
	for (auto& track : mTracks) {
		if (track->GetParent() == nullptr)
			roots.push_back(track);
	}
	// render detector sources first so a sidechain reads this block's audio rather
	// than the previous one. summing is commutative, so reordering the roots cannot
	// change the mix; a cycle (two tracks ducking each other) just leaves the later
	// one reading one block late, which the hub handles by keeping the last buffer
	if (hub.HasSources()) {
		std::stable_partition(roots.begin(), roots.end(),
							  [this](const std::shared_ptr<Track>& t) { return SubtreeFeedsSidechain(t); });
	}

	for (auto& track : roots) {
		ProcessTrackRecursively(track, mMixBuffer.data(), numFrames, numChannels, context, liveMIDIEvents, anySolo);
	}

	if (mMasterTrack) {
		for (int i = 0; i < numFrames * numChannels; ++i)
			destinationBuffer[i] = mMixBuffer[i];
		std::vector<MIDIMessage> emptyMIDI;
		mMasterTrack->Process(destinationBuffer, numFrames, numChannels, emptyMIDI, context, false);
	} else {
		for (int i = 0; i < numFrames * numChannels; ++i)
			destinationBuffer[i] = mMixBuffer[i];
	}
}

void Project::ProcessBlock(float* outputBuffer, int numFrames, int numChannels, std::vector<MIDIMessage>& liveMIDIEvents) {
	std::lock_guard<std::mutex> lock(mMutex);

	bool isPlaying = mTransport.IsPlaying();
	int64_t blockStartSample = mTransport.GetPosition();

	// flush held/stuck notes when playback stops, and also when the playhead jumps
	// discontinuously while playing (a seek). without this, a clip's pending note-off
	// falls in a block window we skip over, so the instrument keeps sounding until the
	// clip replays that note. contiguous playback advances by exactly numFrames per block,
	// so any mismatch with the previous block's end is a seek (loop wraps and tempo
	// re-derivation happen inside a block, so they don't trip this)
	bool stopped = mWasPlaying && !isPlaying;
	bool startedPlaying = !mWasPlaying && isPlaying;
	bool seeked = isPlaying && mLastBlockEndSample >= 0 && blockStartSample != mLastBlockEndSample;
	if (stopped || seeked) {
		for (auto& track : mTracks)
			track->Reset();
		if (mMasterTrack)
			mMasterTrack->Reset();
	}
	mWasPlaying = isPlaying;

	// beat->sample rounding can place the playhead a sample or two past a note's computed
	// onset, so a note lined up with the playhead would be dropped by the exact window test
	// on the block where we just started or jumped, tell the sequencer to chase those onsets
	bool playheadJumped = startedPlaying || seeked;

	// where this block ends, tracked from our own advance arithmetic rather than read
	// back off the transport at the end. the UI seeks by writing the transport's atomic
	// position without taking the project lock (by design - Transport is all atomics), so
	// a seek landing anywhere inside this block would otherwise be read back here as the
	// expected end. the next block would then see no discontinuity at all: the seek is
	// swallowed, no Reset runs, and anything sounding across it hangs. that race is a
	// whole block wide, which is why seeking mid-note only sometimes left a note stuck
	int64_t blockEndSample = blockStartSample + numFrames;

	if (mMasterTrack) {
		if (auto bpmParam = mMasterTrack->GetBpmParameter()) {
			double newBpm = bpmParam->value;
			if (std::abs(newBpm - mTransport.GetBpm()) > 0.001) {
				SetBpmInternal(newBpm);
			}
		}

		if (isPlaying) {
			double currentBeat = (double)mTransport.GetPosition() / mTransport.GetSampleRate() * (mTransport.GetBpm() / 60.0);
			mMasterTrack->EvaluateAutomation(currentBeat);
			if (auto bpmParam = mMasterTrack->GetBpmParameter()) {
				double newBpm = bpmParam->value;
				if (std::abs(newBpm - mTransport.GetBpm()) > 0.001) {
					SetBpmInternal(newBpm);
				}
			}
		}
	}

	bool anySolo = false;
	for (auto& track : mTracks) {
		if (track->GetSolo()) {
			anySolo = true;
			break;
		}
	}

	// looping logic
	if (isPlaying && mTransport.IsLoopEnabled()) {
		int64_t currentPos = mTransport.GetPosition();
		int64_t loopStart = mTransport.GetLoopStart();
		int64_t loopEnd = mTransport.GetLoopEnd();

		if (loopEnd <= loopStart) { // loop sanity check
			ProcessContext context;
			context.sampleRate = mTransport.GetSampleRate();
			context.currentSample = mTransport.GetPosition();
			context.bpm = mTransport.GetBpm();
			context.isPlaying = isPlaying;
			context.playheadJumped = playheadJumped;
			ProcessAudioGraph(outputBuffer, numFrames, numChannels, context, liveMIDIEvents, anySolo);
			mTransport.Advance(numFrames);
			mLastBlockEndSample = blockEndSample;
			return;
		}

		int framesProcessed = 0;
		while (framesProcessed < numFrames) {
			int64_t pos = mTransport.GetPosition();
			bool wrapped = false;

			if (pos >= loopEnd) { // immediate wrap
				pos = loopStart;
				mTransport.SetPosition(pos);
				wrapped = true;

				// a note held across the loop end has its note-off past loopEnd, which we jump
				// away from, so it would stick. flush held notes on the wrap (notes only, to
				// keep effect delay/reverb tails ringing seamlessly across the loop point)
				for (auto& track : mTracks)
					track->AllNotesOff();
				if (mMasterTrack)
					mMasterTrack->AllNotesOff();
			}

			int64_t framesUntilLoopEnd = loopEnd - pos;
			if (pos < loopStart && framesUntilLoopEnd < 0) {
				framesUntilLoopEnd = numFrames; // fail-safe
			}

			int chunk = std::min((int)(numFrames - framesProcessed), (int)framesUntilLoopEnd);
			ProcessContext context;
			context.sampleRate = mTransport.GetSampleRate();
			context.currentSample = pos;
			context.bpm = mTransport.GetBpm();
			context.isPlaying = isPlaying;
			// a wrapped chunk restarts at the loop start, and the very first chunk begins at a
			// jumped-to position; both are jumps that should chase onsets rounding just before them
			context.playheadJumped = wrapped || (framesProcessed == 0 && playheadJumped);

			float* outPtr = outputBuffer + (framesProcessed * numChannels); // offset output
			// mIDI frame timing warning: splitting MIDI during wrap is complex
			ProcessAudioGraph(outPtr, chunk, numChannels, context, (framesProcessed == 0 ? liveMIDIEvents : std::vector<MIDIMessage>{}), anySolo);

			mTransport.Advance(chunk);
			blockEndSample = pos + chunk; // a wrap moves the end with it
			framesProcessed += chunk;
		}

	} else {
		ProcessContext context;
		context.sampleRate = mTransport.GetSampleRate();
		context.currentSample = mTransport.GetPosition();
		context.bpm = mTransport.GetBpm();
		context.isPlaying = isPlaying;
		context.playheadJumped = playheadJumped;

		ProcessAudioGraph(outputBuffer, numFrames, numChannels, context, liveMIDIEvents, anySolo);
		mTransport.Advance(numFrames);
	}

	mLastBlockEndSample = blockEndSample;
}

struct WavHeader {
	char riff[4] = {'R', 'I', 'F', 'F'};
	uint32_t overallSize = 0;
	char wave[4] = {'W', 'A', 'V', 'E'};
	char fmt[4] = {'f', 'm', 't', ' '};
	uint32_t fmtChunkSize = 16;
	uint16_t formatType = 1;
	uint16_t channels = 2;
	uint32_t sampleRate = 44100;
	uint32_t byteRate = 0;
	uint16_t blockAlign = 0;
	uint16_t bitsPerSample = 16;
	char data[4] = {'d', 'a', 't', 'a'};
	uint32_t dataSize = 0;
};

// beats of silence appended past the last clip, so a reverb or delay still ringing at
// the end of the arrangement is not cut off mid-tail
static constexpr double kRenderTailBeats = 4.0;

double Project::SubtreeEndBeat(const std::shared_ptr<Track>& root) const {
	double endBeat = 0.0;
	for (const auto& t : mTracks) {
		if (root && !IsInSubtree(t, root))
			continue;
		for (const auto& c : t->GetClips())
			endBeat = std::max(endBeat, c->GetEndBeat());
	}
	return endBeat;
}

bool Project::RenderAudio(const std::string& path, double startBeat, double endBeat, double sampleRate) {
	std::lock_guard<std::mutex> lock(mMutex);
	return RenderToWav(path, nullptr, startBeat, endBeat, sampleRate);
}

bool Project::RenderToWav(const std::string& path, const std::shared_ptr<Track>& track,
						  double startBeat, double endBeat, double sampleRate) {
	if (endBeat <= startBeat) { // detect max duration
		endBeat = SubtreeEndBeat(track) + kRenderTailBeats;
		startBeat = 0.0;
	}

	double durationBeats = endBeat - startBeat;
	if (durationBeats <= 0)
		return false;

	double seconds = durationBeats * (60.0 / mTransport.GetBpm());
	int64_t totalFrames = (int64_t)(seconds * sampleRate);
	int64_t startFrame = (int64_t)(startBeat * (60.0 / mTransport.GetBpm()) * sampleRate);

	std::ofstream outFile(path, std::ios::binary);
	if (!outFile.is_open())
		return false;

	WavHeader header;
	header.sampleRate = (uint32_t)sampleRate;
	header.channels = 2;
	header.bitsPerSample = 16;
	header.blockAlign = header.channels * (header.bitsPerSample / 8);
	header.byteRate = header.sampleRate * header.blockAlign;
	header.dataSize = (uint32_t)(totalFrames * header.blockAlign);
	header.overallSize = header.dataSize + 36;

	outFile.write((char*)&header, sizeof(WavHeader));

	// save transport state
	double oldSR = mTransport.GetSampleRate();
	int64_t oldPos = mTransport.GetPosition();
	bool oldPlaying = mTransport.IsPlaying();
	bool oldLoop = mTransport.IsLoopEnabled();

	// render setup
	mTransport.SetSampleRate(sampleRate);
	mTransport.SetPosition(startFrame);
	mTransport.SetPlaying(true);
	mTransport.SetLoopEnabled(false); // disable looping

	PrepareToPlayInternal(sampleRate);
	for (auto& track : mTracks) {
		track->Reset();
	}
	if (mMasterTrack)
		mMasterTrack->Reset();

	const int blockSize = 512;
	std::vector<float> blockBuffer(blockSize * 2);
	std::vector<int16_t> intBuffer(blockSize * 2);
	// where a single-track bounce throws away the sidechain sources it still has to run
	std::vector<float> detectorBuffer(blockSize * 2);
	std::vector<MIDIMessage> emptyMIDI;

	int64_t framesRemaining = totalFrames;
	bool anySolo = false;
	for (auto& track : mTracks) {
		if (track->GetSolo()) {
			anySolo = true;
			break;
		}
	}

	bool firstRenderBlock = true;
	while (framesRemaining > 0) {
		int framesToDo = (framesRemaining > blockSize) ? blockSize : (int)framesRemaining;

		ProcessContext context;
		context.sampleRate = sampleRate;
		context.currentSample = mTransport.GetPosition();
		context.bpm = mTransport.GetBpm();
		context.isPlaying = true;
		// the export begins at startFrame; chase onsets that round to just before it
		context.playheadJumped = firstRenderBlock;
		firstRenderBlock = false;

		std::fill(blockBuffer.begin(), blockBuffer.end(), 0.0f);
		if (track) {
			// the subtree walk on its own, so the detector slots still decay per block
			// the way ProcessAudioGraph would have kept them. anySolo is false: a solo
			// somewhere else in the project has nothing to say about a bounce of this
			// track, and its own mute was already lifted by the caller
			SidechainHub& hub = SidechainHub::Instance();
			hub.BeginBlock(framesToDo);

			// a bounce of a ducked track has to hear the thing ducking it, or it comes
			// out flat and silently wrong. every detector source outside the subtree is
			// rendered first into a buffer thrown away, which is the producer-before-
			// consumer order ProcessAudioGraph keeps, minus the summing. a source that
			// overlaps the subtree either way is skipped: it is already being rendered
			// as part of the bounce, and running it twice in one block would advance its
			// dsp twice
			if (hub.HasSources()) {
				for (auto& source : mTracks) {
					if (!hub.IsSource(source->GetId()))
						continue;
					if (IsInSubtree(source, track) || IsInSubtree(track, source))
						continue;
					std::fill(detectorBuffer.begin(), detectorBuffer.end(), 0.0f);
					ProcessTrackRecursively(source, detectorBuffer.data(), framesToDo, 2, context, emptyMIDI, false);
				}
			}

			ProcessTrackRecursively(track, blockBuffer.data(), framesToDo, 2, context, emptyMIDI, false);
		} else {
			ProcessAudioGraph(blockBuffer.data(), framesToDo, 2, context, emptyMIDI, anySolo);
		}
		mTransport.Advance(framesToDo);

		// float to int16 conversion
		for (int i = 0; i < framesToDo * 2; ++i) {
			float val = blockBuffer[i];
			if (val > 1.0f)
				val = 1.0f;
			if (val < -1.0f)
				val = -1.0f;
			intBuffer[i] = (int16_t)(val * 32767.0f);
		}

		outFile.write((char*)intBuffer.data(), framesToDo * 2 * sizeof(int16_t));
		framesRemaining -= framesToDo;
	}

	mTransport.SetSampleRate(oldSR);
	mTransport.SetPosition(oldPos);
	mTransport.SetPlaying(oldPlaying);
	mTransport.SetLoopEnabled(oldLoop);
	PrepareToPlayInternal(oldSR); // internal sr reset

	return true;
}

// a track name goes into a filename, and track names are free text. anything a path
// cannot carry becomes an underscore rather than a failed open
static std::string SanitizeForFilename(const std::string& name) {
	std::string out;
	out.reserve(name.size());
	for (char c : name) {
		const bool safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '-' || c == '_';
		out.push_back(safe ? c : '_');
	}
	// a name that was entirely punctuation would leave nothing to open
	if (out.find_first_not_of(' ') == std::string::npos)
		out = "Track";
	return out;
}

std::shared_ptr<Track> Project::RenderTrackToNewTrack(int index, const std::string& directory,
													  double startBeat, double endBeat) {
	std::lock_guard<std::mutex> lock(mMutex);

	if (index < 0 || index >= (int)mTracks.size())
		return nullptr;
	auto source = mTracks[index];

	// an explicit range wins - that is the time selection the user drew. otherwise the
	// track's own content decides how far the bounce runs, plus the usual device tail
	if (endBeat <= startBeat) {
		double contentEnd = SubtreeEndBeat(source);
		if (contentEnd <= 0.0)
			return nullptr; // nothing under this track to bounce
		startBeat = 0.0;
		endBeat = contentEnd + kRenderTailBeats;
	}

	std::error_code ec;
	std::filesystem::create_directories(directory, ec);

	// a track bounced twice must not overwrite its own first take, which a clip
	// elsewhere in the project may still be pointing at
	const std::string stem = SanitizeForFilename(source->GetName()) + " Render";
	std::filesystem::path path = std::filesystem::path(directory) / (stem + ".wav");
	for (int n = 2; std::filesystem::exists(path, ec) && n < 1000; ++n)
		path = std::filesystem::path(directory) / (stem + " " + std::to_string(n) + ".wav");

	const double sampleRate = mTransport.GetSampleRate() > 0 ? mTransport.GetSampleRate() : 48000.0;

	// see the header: mute and solo describe what the mix is doing right now, not what
	// this track is, and a bounce has to hear the track even when the mix does not
	const bool sourceMuted = source->GetMute();
	source->SetMute(false);
	const bool rendered = RenderToWav(path.string(), source, startBeat, endBeat, sampleRate);
	source->SetMute(sourceMuted);

	if (!rendered)
		return nullptr;

	auto clip = std::make_shared<AudioClip>();
	if (!clip->LoadFromFile(path.string()))
		return nullptr;

	clip->SetName(stem);
	clip->SetStartBeat(startBeat);
	clip->SetDuration(endBeat - startBeat);
	// the wav is a whole number of frames, so the beats it covers can land a hair under
	// what was asked for; clamping is what keeps the clip from ending in a sliver of
	// silence past the file
	clip->ValidateDuration(mTransport.GetBpm());

	auto destination = CreateTrackAfterInternal(index);
	destination->SetName(stem);
	destination->SetColor(source->GetColor());
	destination->AddClip(clip);
	return destination;
}

void Project::Save(const std::string& path) {
	std::lock_guard<std::mutex> lock(mMutex);
	std::ofstream out(path);
	if (!out.is_open())
		return;

	out << "PROJECT_BEGIN\n";
	out << "VERSION " << kCurrentProjectVersion << "\n";
	out << "BPM " << mTransport.GetBpm() << "\n";

	double sR = mTransport.GetSampleRate() > 0 ? mTransport.GetSampleRate() : 48000.0;
	double beatsPerSec = mTransport.GetBpm() / 60.0;

	out << "PLAYHEAD_BEAT " << ((double)mTransport.GetPosition() / sR * beatsPerSec) << "\n";
	out << "LOOP_EN " << (mTransport.IsLoopEnabled() ? 1 : 0) << "\n";
	out << "LOOP_START_BEAT " << ((double)mTransport.GetLoopStart() / sR * beatsPerSec) << "\n";
	out << "LOOP_END_BEAT " << ((double)mTransport.GetLoopEnd() / sR * beatsPerSec) << "\n";
	out << "VIEW_PPB " << mViewState.pixelsPerBeat << "\n";
	out << "VIEW_SEL_START " << mViewState.selectionStart << "\n";
	out << "VIEW_SEL_END " << mViewState.selectionEnd << "\n";
	out << "VIEW_SCROLL_X " << mViewState.scrollX << "\n";
	out << "VIEW_SCROLL_Y " << mViewState.scrollY << "\n";
	out << "VIEW_GRID_NUM " << mViewState.timelineGridNumerator << "\n";
	out << "VIEW_GRID_DEN " << mViewState.timelineGridDenominator << "\n";

	for (int i = 0; i < (int)mTracks.size(); ++i) {
		mTracks[i]->Save(out, i);
		if (auto p = mTracks[i]->GetParent()) {
			int pIdx = -1;
			for (int k = 0; k < (int)mTracks.size(); ++k) {
				if (mTracks[k] == p) {
					pIdx = k;
					break;
				}
			}
			out << "PARENT_IDX " << pIdx << "\n";
		}
	}

	if (mMasterTrack) {
		out << "MASTER_BEGIN\n";
		mMasterTrack->Save(out, -1);
		out << "MASTER_END\n";
	}

	out << "PROJECT_END\n";
}

void Project::RelinkMIDIClips() {
	// the first clip carrying a given saved id keeps the sequence it parsed and every
	// later clip with that id adopts it, so the notes exist once and all of them edit
	// the same vector again. an id of 0 is a clip from before SEQ was written, or one
	// that was genuinely unique; either way it is left alone
	std::unordered_map<uint32_t, std::shared_ptr<MIDISequence>> byLoadedId;

	auto relinkTrack = [&](const std::shared_ptr<Track>& track) {
		if (!track)
			return;
		for (auto& clip : track->GetClips()) {
			auto mIDIClip = std::dynamic_pointer_cast<MIDIClip>(clip);
			if (!mIDIClip || mIDIClip->GetLoadedSequenceId() == 0)
				continue;
			auto& slot = byLoadedId[mIDIClip->GetLoadedSequenceId()];
			if (slot)
				mIDIClip->AdoptSequence(slot);
			else
				slot = mIDIClip->GetSequence();
		}
	};

	for (auto& t : mTracks)
		relinkTrack(t);
	relinkTrack(mMasterTrack);
}

void Project::Load(const std::string& path) {
	std::lock_guard<std::mutex> lock(mMutex);
	// through a path rather than the narrow string: a project opened from the library
	// explorer arrives as UTF-8, which a narrow ifstream would read in the ANSI code page
	std::ifstream in(PathText::ToPath(path));
	if (!in.is_open())
		return;

	mTracks.clear();
	if (mMasterTrack)
		mMasterTrack->Reset();

	std::string line;
	int version = 0;

	double loadedPlayheadBeat = 0.0;
	double loadedLoopStartBeat = 0.0;
	double loadedLoopEndBeat = 4.0;
	bool loadedLoopEn = false;

	mViewState = ProjectViewState(); // reset to default

	while (std::getline(in, line)) {
		if (line == "PROJECT_END")
			break;

		std::stringstream ss(line);
		std::string token;
		ss >> token;

		if (token == "VERSION") {
			ss >> version;
		} else if (token == "BPM") {
			double bpm;
			ss >> bpm;
			mTransport.SetBpm(bpm);
		} else if (token == "PLAYHEAD_BEAT") {
			ss >> loadedPlayheadBeat;
		} else if (token == "LOOP_EN") {
			int val;
			ss >> val;
			loadedLoopEn = (val != 0);
		} else if (token == "LOOP_START_BEAT") {
			ss >> loadedLoopStartBeat;
		} else if (token == "LOOP_END_BEAT") {
			ss >> loadedLoopEndBeat;
		} else if (token == "VIEW_PPB") {
			ss >> mViewState.pixelsPerBeat;
		} else if (token == "VIEW_SEL_START") {
			ss >> mViewState.selectionStart;
		} else if (token == "VIEW_SEL_END") {
			ss >> mViewState.selectionEnd;
		} else if (token == "VIEW_SCROLL_X") {
			ss >> mViewState.scrollX;
		} else if (token == "VIEW_SCROLL_Y") {
			ss >> mViewState.scrollY;
		} else if (token == "VIEW_GRID_NUM") {
			ss >> mViewState.timelineGridNumerator;
		} else if (token == "VIEW_GRID_DEN") {
			ss >> mViewState.timelineGridDenominator;
		} else if (token == "TRACK_BEGIN") {
			auto t = std::make_shared<Track>();
			t->Load(in);
			if (mTransport.GetSampleRate() > 0)
				t->PrepareToPlay(mTransport.GetSampleRate());
			mTracks.push_back(t);
		} else if (token == "MASTER_BEGIN") {
			mMasterTrack = std::make_shared<Track>();
			mMasterTrack->SetName("Master");
			mMasterTrack->InitMasterTrackParameters(mTransport.GetBpm());
			mMasterTrack->Load(in); // master track scope
			std::string endTag;
			std::getline(in, endTag); // consume MASTER_END
			if (mTransport.GetSampleRate() > 0)
				mMasterTrack->PrepareToPlay(mTransport.GetSampleRate());
		} else if (token == "PARENT_IDX") {
			int pIdx = -1;
			ss >> pIdx;
			if (!mTracks.empty()) {
				mTracks.back()->mLoadedParentIndex = pIdx; // hierarchy restoration
			}
		}
	}

	for (auto& t : mTracks) {
		t->RebindAutomation();
		if (t->mLoadedParentIndex != -1 && t->mLoadedParentIndex < (int)mTracks.size()) {
			t->SetParent(mTracks[t->mLoadedParentIndex]);
		}
	}
	if (mMasterTrack)
		mMasterTrack->RebindAutomation();

	RelinkMIDIClips();

	double sR = mTransport.GetSampleRate() > 0 ? mTransport.GetSampleRate() : 48000.0;
	double secsPerBeat = 60.0 / mTransport.GetBpm();

	mTransport.SetPosition((int64_t)(loadedPlayheadBeat * secsPerBeat * sR));
	mTransport.SetLoopRange((int64_t)(loadedLoopStartBeat * secsPerBeat * sR), (int64_t)(loadedLoopEndBeat * secsPerBeat * sR));
	mTransport.SetLoopEnabled(loadedLoopEn);
}
