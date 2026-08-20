#include "PrecompHeader.h"
#include "SidechainHub.h"
#include <algorithm>

SidechainHub& SidechainHub::Instance() {
	static SidechainHub instance;
	return instance;
}

void SidechainHub::SetProject(Project* project) {
	std::lock_guard<std::mutex> lock(mMutex);
	mProject = project;
}

Project* SidechainHub::GetProject() const {
	std::lock_guard<std::mutex> lock(mMutex);
	return mProject;
}

void SidechainHub::Subscribe(uint32_t trackId) {
	if (trackId == 0)
		return;
	std::lock_guard<std::mutex> lock(mMutex);
	auto& slot = mSlots[trackId];
	if (!slot)
		slot = std::make_unique<Slot>();
	if (slot->refCount == 0)
		mLiveSources++;
	slot->refCount++;
}

void SidechainHub::Unsubscribe(uint32_t trackId) {
	if (trackId == 0)
		return;
	std::lock_guard<std::mutex> lock(mMutex);
	auto it = mSlots.find(trackId);
	if (it == mSlots.end() || it->second->refCount <= 0)
		return;
	it->second->refCount--;
	if (it->second->refCount == 0) {
		mLiveSources--;
		// keep the slot (see header) but stop it feeding a stale signal to a plugin
		// that re-subscribes to this id later
		std::fill(it->second->mono.begin(), it->second->mono.end(), 0.0f);
	}
}

bool SidechainHub::HasSources() const {
	std::lock_guard<std::mutex> lock(mMutex);
	return mLiveSources > 0;
}

bool SidechainHub::IsSource(uint32_t trackId) const {
	if (trackId == 0)
		return false;
	std::lock_guard<std::mutex> lock(mMutex);
	auto it = mSlots.find(trackId);
	return it != mSlots.end() && it->second->refCount > 0;
}

void SidechainHub::BeginBlock(int numFrames) {
	if (numFrames <= 0)
		return;
	std::lock_guard<std::mutex> lock(mMutex);
	mBlockFrames = numFrames;
	for (auto& entry : mSlots) {
		Slot& slot = *entry.second;
		if ((int)slot.mono.size() < numFrames)
			slot.mono.resize(numFrames, 0.0f);
		// a source that produced nothing last block (its track was deleted, or it no
		// longer reaches the graph) must fall silent instead of ducking forever on
		// stale audio. a producer that simply runs after its consumer keeps working:
		// it re-publishes every block, so the consumer only ever sees one block of lag
		if (!slot.published)
			std::fill(slot.mono.begin(), slot.mono.end(), 0.0f);
		slot.published = false;
	}
}

void SidechainHub::Publish(uint32_t trackId, const float* interleaved, int numFrames, int numChannels) {
	if (trackId == 0 || !interleaved || numFrames <= 0 || numChannels <= 0)
		return;
	std::lock_guard<std::mutex> lock(mMutex);
	auto it = mSlots.find(trackId);
	if (it == mSlots.end() || it->second->refCount <= 0)
		return;

	Slot& slot = *it->second;
	if ((int)slot.mono.size() < numFrames)
		slot.mono.resize(numFrames, 0.0f);

	const float norm = 1.0f / (float)numChannels;
	for (int i = 0; i < numFrames; ++i) {
		float sum = 0.0f;
		for (int c = 0; c < numChannels; ++c)
			sum += interleaved[i * numChannels + c];
		slot.mono[i] = sum * norm;
	}
	slot.published = true;
}

const float* SidechainHub::Read(uint32_t trackId) const {
	if (trackId == 0)
		return nullptr;
	std::lock_guard<std::mutex> lock(mMutex);
	auto it = mSlots.find(trackId);
	if (it == mSlots.end() || (int)it->second->mono.size() < mBlockFrames)
		return nullptr;
	return it->second->mono.data();
}
