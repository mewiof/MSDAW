#include "PrecompHeader.h"
#include "SystemMonitor.h"

#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h> // K32GetProcessMemoryInfo + PROCESS_MEMORY_COUNTERS (both live in kernel32, no psapi.lib needed)
#endif

namespace {
	// monotonic wall clock in nanoseconds. steady_clock never jumps backwards, so
	// it is safe to subtract for deltas (unlike wall-clock time-of-day)
	uint64_t NowNs() {
		using namespace std::chrono;
		return (uint64_t)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
	}
} //namespace

SystemMonitor::SystemMonitor() {
#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	mNumProcessors = (int)si.dwNumberOfProcessors;
#endif
	if (mNumProcessors < 1)
		mNumProcessors = 1;
}

void SystemMonitor::Update() {
	uint64_t now = NowNs();

	// throttle everything to the sample interval. the very first call (mLastSampleNs
	// == 0) always falls through so we seed the cpu baseline immediately
	if (mLastSampleNs != 0 && (now - mLastSampleNs) < kSampleIntervalNs)
		return;

#ifdef _WIN32
	// ---- cpu: our process time as a share of the whole machine ----
	// (kernel+user) time is cumulative-since-launch, so we diff two samples and
	// divide by the wall time elapsed. dividing again by the core count matches the
	// task-manager convention where 100% means every logical core fully saturated
	FILETIME creation, exit, kernel, user;
	if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
		ULARGE_INTEGER k, u;
		k.LowPart = kernel.dwLowDateTime;
		k.HighPart = kernel.dwHighDateTime;
		u.LowPart = user.dwLowDateTime;
		u.HighPart = user.dwHighDateTime;
		uint64_t procNs = (uint64_t)(k.QuadPart + u.QuadPart) * 100ull; // FILETIME ticks are 100ns

		if (mLastSampleNs != 0) {
			uint64_t dProc = procNs - mLastProcNs;
			uint64_t dWall = now - mLastSampleNs;
			if (dWall > 0) {
				double pct = (double)dProc / (double)dWall / (double)mNumProcessors * 100.0;
				if (pct < 0.0)
					pct = 0.0;
				if (pct > 100.0)
					pct = 100.0;
				// light smoothing so the number reads steadily instead of twitching
				mCpuPercent = mCpuPercent * 0.4f + (float)pct * 0.6f;
			}
		}
		mLastProcNs = procNs;
	}

	// ---- memory: our working set + the machine's physical totals ----
	SIZE_T workingSet = 0;
	PROCESS_MEMORY_COUNTERS pmc;
	ZeroMemory(&pmc, sizeof(pmc));
	pmc.cb = sizeof(pmc);
	if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
		workingSet = pmc.WorkingSetSize;
		mRamUsedMB = (float)((double)workingSet / (1024.0 * 1024.0));
	}

	MEMORYSTATUSEX ms;
	ZeroMemory(&ms, sizeof(ms));
	ms.dwLength = sizeof(ms);
	if (GlobalMemoryStatusEx(&ms)) {
		mTotalRamMB = (float)((double)ms.ullTotalPhys / (1024.0 * 1024.0));
		mSystemRamLoad = (float)ms.dwMemoryLoad;
		if (ms.ullTotalPhys > 0 && workingSet > 0)
			mRamFraction = (float)((double)workingSet / (double)ms.ullTotalPhys);
	}
#endif

	mLastSampleNs = now;
}
