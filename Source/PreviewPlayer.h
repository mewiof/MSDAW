#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// one-shot audition of an audio file, mixed into the output alongside the project
//
// the library explorer plays a file the moment it is clicked, with the transport
// usually stopped, so this deliberately does not go through the project graph: no
// track, no clip, no undo, nothing serialized and nothing to clean up afterwards
//
// the UI thread reads the file and hands over a finished buffer; the audio thread
// only ever reads what it was handed. every buffer stays referenced by the UI
// thread until it sees the audio thread has let go of it, so releasing one inside
// the callback is an atomic decrement and never a free - unbounded work is exactly
// what a realtime thread must not do
class PreviewPlayer {
public:
	// interleaved samples as they came off disk, at the file's own rate
	struct Buffer {
		std::vector<float> samples;
		int channels = 1;
		double sampleRate = 48000.0;
	};

	// the rate the output stream runs at; anything at a different rate is resampled
	// on the way out
	void SetOutputSampleRate(double rate);

	// ---- ui thread ----

	// audition a file from its start. false when it is not one the WAV reader takes,
	// in which case whatever was playing keeps playing
	bool Play(const std::string& path);
	void Stop();

	// true from the moment a file is handed over until the audio thread runs off the
	// end of it or is told to stop
	bool IsPlaying() const { return mPlaying.load(std::memory_order_relaxed); }

	// the file being auditioned, empty once it has finished. compared against a row
	// in the browser to draw it as the one playing
	const std::string& PlayingPath() const { return mPlayingPath; }

	// let go of the buffers the audio thread has finished with. called once a frame;
	// this is the thread that does the freeing
	void Collect();

	// ---- audio thread ----

	// add the next block of the audition into an interleaved output buffer
	void ProcessBlock(float* out, unsigned int frames, int channels);
private:
	std::mutex mMutex;					   // held briefly, never around the file read
	std::shared_ptr<Buffer> mPending;	   // handed over, not yet picked up
	bool mStopRequested = false;

	std::shared_ptr<Buffer> mActive;	   // audio thread only, once picked up
	double mPosition = 0.0;				   // ... in source frames, fractional while resampling

	std::atomic<bool> mPlaying{false};
	std::string mPlayingPath;			   // UI thread only
	double mOutputSampleRate = 48000.0;

	// everything ever handed over, kept alive here so that the audio thread's own
	// reference is never the last one. entries drop out in Collect
	std::vector<std::shared_ptr<Buffer>> mHandedOver;
};
