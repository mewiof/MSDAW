#include "PrecompHeader.h"
#include "Project.h"
#include "Processors/SimpleSynth.h"
#include "Clips/MIDIClip.h"
#include "Clips/AudioClip.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <functional>

// version history
const int kCurrentProjectVersion = 1; // 1: initial format

Project::Project() {
}

Project::~Project() {
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
	auto track = std::make_shared<Track>();
	track->SetName("Track " + std::to_string(mTracks.size() + 1));

	if (mTransport.GetSampleRate() > 0) {
		track->PrepareToPlay(mTransport.GetSampleRate());
	}
	mTracks.push_back(track);
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

	// 4. validate audio clip tempo
	for (auto& track : mTracks) {
		for (auto& clip : track->GetClips()) {
			if (auto ac = std::dynamic_pointer_cast<AudioClip>(clip)) {
				ac->ValidateDuration(bpm);
			}
		}
	}
}

void Project::SetBpm(double bpm) {
	std::lock_guard<std::mutex> lock(mMutex);
	SetBpmInternal(bpm);
}

void Project::ProcessTrackRecursively(std::shared_ptr<Track> track, float* destinationBuffer, int numFrames, int numChannels, const ProcessContext& context, const std::vector<MIDIMessage>& liveMIDIEvents, bool anySolo) {

	// determine if this track is "effectively soloed".
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

	// must be soloed to bypass mute
	if (track->GetMute() && !isEffectiveSolo)
		return;

	// global check
	if (anySolo && !isEffectiveSolo) {
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
			return;
	}

	if (track->IsGroup()) {
		track->ClearAccumulator();
		for (auto& child : mTracks) {
			if (child->GetParent() == track) {
				std::vector<float> childBuffer(numFrames * numChannels, 0.0f);
				ProcessTrackRecursively(child, childBuffer.data(), numFrames, numChannels, context, liveMIDIEvents, anySolo);
				track->AddToAccumulator(childBuffer.data(), numFrames, numChannels);
			}
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

	track->Process(processBuffer.data(), numFrames, numChannels, trackMIDI, context);

	for (int i = 0; i < numFrames * numChannels; ++i) {
		destinationBuffer[i] += processBuffer[i];
	}
}

void Project::ProcessAudioGraph(float* destinationBuffer, int numFrames, int numChannels, const ProcessContext& context, const std::vector<MIDIMessage>& liveMIDIEvents, bool anySolo) {
	if (mMixBuffer.size() < (size_t)(numFrames * numChannels)) {
		mMixBuffer.resize(numFrames * numChannels);
	}
	std::fill(mMixBuffer.begin(), mMixBuffer.begin() + (numFrames * numChannels), 0.0f);

	for (auto& track : mTracks) {
		if (track->GetParent() == nullptr) {
			ProcessTrackRecursively(track, mMixBuffer.data(), numFrames, numChannels, context, liveMIDIEvents, anySolo);
		}
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
	if (mWasPlaying && !isPlaying) {
		for (auto& track : mTracks)
			track->Reset();
		if (mMasterTrack)
			mMasterTrack->Reset();
	}
	mWasPlaying = isPlaying;

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
			ProcessAudioGraph(outputBuffer, numFrames, numChannels, context, liveMIDIEvents, anySolo);
			mTransport.Advance(numFrames);
			return;
		}

		int framesProcessed = 0;
		while (framesProcessed < numFrames) {
			int64_t pos = mTransport.GetPosition();

			if (pos >= loopEnd) { // immediate wrap
				pos = loopStart;
				mTransport.SetPosition(pos);
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

			float* outPtr = outputBuffer + (framesProcessed * numChannels); // offset output
			// mIDI frame timing warning: splitting MIDI during wrap is complex
			ProcessAudioGraph(outPtr, chunk, numChannels, context, (framesProcessed == 0 ? liveMIDIEvents : std::vector<MIDIMessage>{}), anySolo);

			mTransport.Advance(chunk);
			framesProcessed += chunk;
		}

	} else {
		ProcessContext context;
		context.sampleRate = mTransport.GetSampleRate();
		context.currentSample = mTransport.GetPosition();
		context.bpm = mTransport.GetBpm();
		context.isPlaying = isPlaying;

		ProcessAudioGraph(outputBuffer, numFrames, numChannels, context, liveMIDIEvents, anySolo);
		mTransport.Advance(numFrames);
	}
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

bool Project::RenderAudio(const std::string& path, double startBeat, double endBeat, double sampleRate) {
	std::lock_guard<std::mutex> lock(mMutex);

	if (endBeat <= startBeat) { // detect max duration
		endBeat = 0.0;
		for (const auto& t : mTracks) {
			for (const auto& c : t->GetClips()) {
				double end = c->GetEndBeat();
				if (end > endBeat)
					endBeat = end;
			}
		}
		endBeat += 4.0; // add tail
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
	std::vector<MIDIMessage> emptyMIDI;

	int64_t framesRemaining = totalFrames;
	bool anySolo = false;
	for (auto& track : mTracks) {
		if (track->GetSolo()) {
			anySolo = true;
			break;
		}
	}

	while (framesRemaining > 0) {
		int framesToDo = (framesRemaining > blockSize) ? blockSize : (int)framesRemaining;

		ProcessContext context;
		context.sampleRate = sampleRate;
		context.currentSample = mTransport.GetPosition();
		context.bpm = mTransport.GetBpm();
		context.isPlaying = true;

		std::fill(blockBuffer.begin(), blockBuffer.end(), 0.0f);
		ProcessAudioGraph(blockBuffer.data(), framesToDo, 2, context, emptyMIDI, anySolo);
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

void Project::Load(const std::string& path) {
	std::lock_guard<std::mutex> lock(mMutex);
	std::ifstream in(path);
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

	double sR = mTransport.GetSampleRate() > 0 ? mTransport.GetSampleRate() : 48000.0;
	double secsPerBeat = 60.0 / mTransport.GetBpm();

	mTransport.SetPosition((int64_t)(loadedPlayheadBeat * secsPerBeat * sR));
	mTransport.SetLoopRange((int64_t)(loadedLoopStartBeat * secsPerBeat * sR), (int64_t)(loadedLoopEndBeat * secsPerBeat * sR));
	mTransport.SetLoopEnabled(loadedLoopEn);
}
