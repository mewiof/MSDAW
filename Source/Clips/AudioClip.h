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
// edit clamps as a side effect (duration/offset), so one edit undoes as one step
// used both as the undo payload and as the "before" grabbed when a drag begins
struct AudioClipWarpState {
	bool warpingEnabled = false;
	WarpMode warpMode = WarpMode::Beats;
	double segmentBpm = 120.0;
	double transposeSemitones = 0.0;
	double transposeCents = 0.0;
	double grainSizeMs = 80.0; // per-mode granular knobs ride along so one edit undoes cleanly
	double fluctuation = 0.0;
	double transientEnvelope = 0.5;
	double formants = 1.0;
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

	// per-mode granular controls (mirror Ableton's per-warp-mode knobs)
	void SetGrainSizeMs(double ms) { mGrainSizeMs = ms; }
	double GetGrainSizeMs() const { return mGrainSizeMs; }

	void SetFluctuation(double f) { mFluctuation = f; }
	double GetFluctuation() const { return mFluctuation; }

	void SetTransientEnvelope(double e) { mTransientEnvelope = e; }
	double GetTransientEnvelope() const { return mTransientEnvelope; }

	void SetFormants(double f) { mFormants = f; }
	double GetFormants() const { return mFormants; }

	// true when the granular time-stretch engine drives playback, i.e. warped in any mode
	// except Re-Pitch (which is intentionally tape-style resampling and bypasses the engine)
	bool UsesGranularEngine() const { return mWarpingEnabled && mWarpMode != WarpMode::RePitch; }

	// helper to calculate max duration in beats
	double GetMaxDurationInBeats(double projectBpm) const;
	// clamps duration to file end based on project bpm
	void ValidateDuration(double projectBpm);

	// re-reads the clip's length on the grid after a project tempo change
	void RetimeForBpmChange(double oldBpm, double newBpm);

	// re-reads the clip's window on the grid after a warp or pitch edit, given the
	// GetMaxDurationInBeats() read just before that edit. pitch on an unwarped (or
	// Re-Pitch) clip is tape speed, so the slice of audio the clip was cut to now takes
	// a different number of beats to play and both ends of the window move with it
	void RetimeForWarpChange(double oldMaxBeats, double projectBpm);

	// source frames advanced per output sample: base resample (file->device) times
	// the warp stretch (project/segment bpm, only when warped) times the pitch factor
	// the single place varispeed playback speed is decided (Re-Pitch and unwarped), so the
	// waveform preview and the audio thread never drift. granular modes split it into the two
	// rates below so time and pitch move independently
	double ComputePlaybackRate(double deviceSampleRate, double projectBpm) const;

	// time base for the granular engine: resample times warp, but NOT pitch. this is what the
	// grain anchor advances at, so it governs duration/grid-sync while pitch stays put
	double ComputeTimeStretchRate(double deviceSampleRate, double projectBpm) const;

	// grain-internal read rate: resample times pitch, but NOT warp. reading the source at this
	// rate inside a grain shifts pitch without touching the clip's length on the grid
	double ComputePitchReadRate(double deviceSampleRate) const;

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

	// per-mode granular controls
	double mGrainSizeMs = 80.0;		 // Tones/Texture/Complex grain length
	double mFluctuation = 0.0;		 // texture randomization, 0..1
	double mTransientEnvelope = 0.5; // beats grain fade shaping, 0..1
	double mFormants = 1.0;			 // ComplexPro formant compensation, 0..1
};
