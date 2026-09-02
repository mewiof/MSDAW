#include "PrecompHeader.h"
#include "PreviewPlayer.h"

#include "Clips/AudioClip.h"

#include <algorithm>

void PreviewPlayer::SetOutputSampleRate(double rate) {
	// set once, while the stream is being opened and before the callback can run
	if (rate > 0.0)
		mOutputSampleRate = rate;
}

// ================================================================
// UI THREAD
// ================================================================

bool PreviewPlayer::Play(const std::string& path) {
	// the file is read here rather than on the audio thread, which is the whole
	// point of the handover below
	AudioClip source;
	if (!source.LoadFromFile(path) || source.GetSamples().empty())
		return false;

	auto buffer = std::make_shared<Buffer>();
	buffer->samples = source.GetSamples();
	buffer->channels = std::max(source.GetNumChannels(), 1);
	buffer->sampleRate = source.GetSampleRate() > 0.0 ? source.GetSampleRate() : mOutputSampleRate;

	mHandedOver.push_back(buffer);
	mPlayingPath = path;
	mPlaying.store(true, std::memory_order_relaxed);

	std::lock_guard<std::mutex> lock(mMutex);
	mPending = std::move(buffer);
	mStopRequested = false;
	return true;
}

void PreviewPlayer::Stop() {
	mPlayingPath.clear();
	mPlaying.store(false, std::memory_order_relaxed);

	std::lock_guard<std::mutex> lock(mMutex);
	mPending.reset();
	mStopRequested = true;
}

void PreviewPlayer::Collect() {
	// the audition ran off the end of the file on the audio thread; the UI side of
	// "what is playing" catches up here
	if (!mPlaying.load(std::memory_order_relaxed))
		mPlayingPath.clear();

	// a buffer nothing but this vector still references is one the audio thread has
	// let go of, so it is this thread that runs the destructor
	mHandedOver.erase(std::remove_if(mHandedOver.begin(), mHandedOver.end(),
									 [](const std::shared_ptr<Buffer>& buffer) { return buffer.use_count() == 1; }),
					  mHandedOver.end());
}

// ================================================================
// AUDIO THREAD
// ================================================================

void PreviewPlayer::ProcessBlock(float* out, unsigned int frames, int channels) {
	// pick up whatever the UI handed over. try_lock rather than lock: if the UI
	// thread happens to be mid-handover this block plays what it already had, and
	// the new file starts one buffer later - inaudible, and worth not blocking for
	{
		std::unique_lock<std::mutex> lock(mMutex, std::try_to_lock);
		if (lock.owns_lock()) {
			if (mStopRequested) {
				mActive.reset();
				mStopRequested = false;
			}
			if (mPending) {
				mActive = std::move(mPending);
				mPosition = 0.0;
			}
		}
	}

	if (!mActive || channels <= 0)
		return;

	const Buffer& buffer = *mActive;
	const int sourceChannels = buffer.channels;
	const size_t totalFrames = buffer.samples.size() / (size_t)sourceChannels;
	if (totalFrames == 0) {
		mActive.reset();
		mPlaying.store(false, std::memory_order_relaxed);
		return;
	}

	// a file recorded at another rate plays at the right pitch by stepping through it
	// at the ratio of the two, interpolating between the frames either side
	const double step = buffer.sampleRate / mOutputSampleRate;

	for (unsigned int frame = 0; frame < frames; ++frame) {
		if (mPosition >= (double)totalFrames) {
			mActive.reset();
			mPlaying.store(false, std::memory_order_relaxed);
			return;
		}

		const size_t index = (size_t)mPosition;
		const size_t next = std::min(index + 1, totalFrames - 1);
		const float fraction = (float)(mPosition - (double)index);

		// mono is heard on both sides; anything wider is auditioned on its first two
		// channels, which is what the timeline draws too
		const size_t left = index * (size_t)sourceChannels;
		const size_t leftNext = next * (size_t)sourceChannels;
		const size_t rightOffset = sourceChannels > 1 ? 1 : 0;

		const float sampleLeft = buffer.samples[left] + (buffer.samples[leftNext] - buffer.samples[left]) * fraction;
		const float sampleRight = buffer.samples[left + rightOffset] +
								  (buffer.samples[leftNext + rightOffset] - buffer.samples[left + rightOffset]) * fraction;

		float* target = out + (size_t)frame * (size_t)channels;
		target[0] += sampleLeft;
		if (channels > 1)
			target[1] += sampleRight;

		mPosition += step;
	}
}
