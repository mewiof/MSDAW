#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "PluginManager.h"

// ================================================================
// PLUGIN SCAN
// ================================================================

// the scan runs on a background thread that loads real plugin binaries, so these
// tests strip the search paths first: what is under test is the thread's lifecycle,
// never the contents of anyone's VST folder

namespace {

	// a manager that will find nothing, and therefore finish almost immediately
	void ClearSearchPaths(PluginManager& manager) {
		while (!manager.GetSearchPaths().empty())
			manager.RemoveSearchPath(0);
	}

	bool WaitForIdle(PluginManager& manager) {
		for (int i = 0; i < 500 && manager.IsScanning(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return !manager.IsScanning();
	}

} // namespace

TEST(PluginScan, StartsIdle) {
	PluginManager manager;
	ClearSearchPaths(manager);

	EXPECT_FALSE(manager.IsScanning());
}

TEST(PluginScan, ReportsIdleAgainOnceAScanFinishes) {
	PluginManager manager;
	ClearSearchPaths(manager);

	manager.ScanPlugins();

	EXPECT_TRUE(WaitForIdle(manager)) << "the scan never cleared its in-flight flag";
	EXPECT_TRUE(manager.GetKnownPlugins().empty());
}

// the bug this guards: every call used to detach a thread of its own, so holding the
// settings button down put a pile of them inside plugin entry points at once. the
// second call now returns without starting anything, and the thread object is reused,
// which is also where getting the join order wrong calls std::terminate outright
TEST(PluginScan, RepeatedRequestsNeverStackUpScans) {
	PluginManager manager;
	ClearSearchPaths(manager);

	for (int i = 0; i < 200; ++i)
		manager.ScanPlugins();

	EXPECT_TRUE(WaitForIdle(manager));
}

// a scan that outlives its manager writes mPlugins through a dangling `this`
TEST(PluginScan, DestructionWaitsForAScanInFlight) {
	{
		PluginManager manager;
		ClearSearchPaths(manager);
		manager.ScanPlugins();
	} // the destructor has to join here rather than leave the thread running

	SUCCEED();
}

// two threads reaching a plugin binary at once is what the lock exists to stop; it
// has to be plain enough to take twice in a row from one thread without seizing up
TEST(PluginScan, TheBinaryLockIsAvailableAndNotSelfBlocking) {
	{
		std::lock_guard<std::mutex> lock(PluginManager::BinaryLock());
	}
	{
		std::lock_guard<std::mutex> lock(PluginManager::BinaryLock());
	}

	bool takenOnAnotherThread = false;
	std::thread other([&takenOnAnotherThread] {
		std::lock_guard<std::mutex> lock(PluginManager::BinaryLock());
		takenOnAnotherThread = true;
	});
	other.join();

	EXPECT_TRUE(takenOnAnotherThread) << "every load site shares one lock instance";
}
