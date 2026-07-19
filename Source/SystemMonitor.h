#pragma once
#include <cstdint>

// samples this process's cpu and memory footprint for the resource meter in the
// top-right of the menu bar. everything here runs on the ui thread and is polled
// once per frame (internally throttled) - it is not realtime-safe and must never
// be touched from the audio thread
class SystemMonitor {
public:
	SystemMonitor();

	// refresh the cached readings, but no more often than the sample interval.
	// cheap to call every frame - most calls just early-out
	void Update();

	float GetCpuPercent() const { return mCpuPercent; }		  // 0..100, share of the whole machine
	float GetRamUsedMB() const { return mRamUsedMB; }		  // process working set, MB
	float GetRamFraction() const { return mRamFraction; }	  // working set / total physical, 0..1
	float GetTotalRamMB() const { return mTotalRamMB; }		  // installed physical ram, MB
	float GetSystemRamLoad() const { return mSystemRamLoad; } // 0..100, whole-system memory pressure
	int GetProcessorCount() const { return mNumProcessors; }
private:
	// recompute at ~2 Hz. a cpu delta measured across a single 60fps frame is
	// mostly tick-quantization noise (the kernel accounts process time in coarse
	// ~15ms slices), so the reading would jitter wildly. cached values hold
	// steady between samples
	static constexpr uint64_t kSampleIntervalNs = 500'000'000ull;

	int mNumProcessors = 1;

	// previous cpu sample, needed to form the busy-time delta
	uint64_t mLastProcNs = 0;	// cumulative kernel+user process time, nanoseconds
	uint64_t mLastSampleNs = 0; // wall clock at the last sample, nanoseconds (0 = never sampled)

	float mCpuPercent = 0.0f;
	float mRamUsedMB = 0.0f;
	float mRamFraction = 0.0f;
	float mTotalRamMB = 0.0f;
	float mSystemRamLoad = 0.0f;
};
