#pragma once
#include "Clip.h"
#include <vector>
#include <string>

enum class WarpMode {
	Beats = 0,
	Tones,
	Texture,
	RePitch,
	Complex,
	ComplexPro
};

// snapshot of everything a warp/pitch edit can touch, including the geometry the
// edit clamps as a side effect (duration/offset), so one edit undoes as one step.
// used both as the undo payload and as the "before" grabbed when a drag begins
struct AudioClipWarpState {
	bool warpingEnabled = false;
	WarpMode warpMode = WarpMode::Beats;
	double segmentBpm = 120.0;
	double transposeSemitones = 0.0;
	double transposeCents = 0.0;
	double duration = 4.0; // clamping is part of the edit, so it rides along
	double offset = 0.0;
};

class AudioClip : public Clip {
public:
	AudioClip();
	~AudioClip() override = default;

	// simple wav loader (supports 16-bit pcm and 32-bit float)
	bool LoadFromFile(const std::string& path);

	// generate a test tone for demonstration
	void GenerateTestSignal(double sampleRate, double durationSecs);

	// access raw interleaved samples
	const std::vector<float>& GetSamples() const { return mSamples; }
	int GetNumChannels() const { return mChannels; }
	double GetSampleRate() const { return mSampleRate; }
	uint64_t GetTotalFileFrames() const { return mTotalFileFrames; }

	// warping and pitch properties
	void SetWarpingEnabled(bool enabled) { mWarpingEnabled = enabled; }
	bool IsWarpingEnabled() const { return mWarpingEnabled; }

	void SetWarpMode(WarpMode mode) { mWarpMode = mode; }
	WarpMode GetWarpMode() const { return mWarpMode; }

	void SetSegmentBpm(double bpm) { mSegmentBpm = bpm; }
	double GetSegmentBpm() const { return mSegmentBpm; }

	void SetTransposeSemitones(double semitones) { mTransposeSemitones = semitones; }
	double GetTransposeSemitones() const { return mTransposeSemitones; }

	void SetTransposeCents(double cents) { mTransposeCents = cents; }
	double GetTransposeCents() const { return mTransposeCents; }

	// helper to calculate max duration in beats
	double GetMaxDurationInBeats(double projectBpm) const;
	// clamps duration to file end based on project bpm
	void ValidateDuration(double projectBpm);

	// source frames advanced per output sample: base resample (file->device) times
	// the warp stretch (project/segment bpm, only when warped) times the pitch factor.
	// the single place playback speed is decided, so the waveform preview and the
	// audio thread never drift; a future time-stretcher would branch off this
	double ComputePlaybackRate(double deviceSampleRate, double projectBpm) const;

	// warp/pitch state snapshot for undo and for grabbing a drag's "before"
	AudioClipWarpState CaptureWarpState() const;
	void ApplyWarpState(const AudioClipWarpState& state);

	void Save(std::ostream& out) override;
	void Load(std::istream& in) override;
private:
	std::vector<float> mSamples; // interleaved data
	int mChannels = 2;
	double mSampleRate = 48000.0;
	uint64_t mTotalFileFrames = 0;
	std::string mFilePath;

	// warping state
	bool mWarpingEnabled = false;
	WarpMode mWarpMode = WarpMode::Beats;
	double mSegmentBpm = 120.0;
	double mTransposeSemitones = 0.0;
	double mTransposeCents = 0.0;
};
