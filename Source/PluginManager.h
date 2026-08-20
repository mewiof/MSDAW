#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct PluginInfo {
	std::string name;
	std::string path;
	bool isSynth;
	std::string vendor;
	std::string format;	 // "VST2" or "VST3"
	std::string classID; // for VST3
};

class PluginManager {
public:
	PluginManager();
	~PluginManager();

	// add VST search path
	void AddSearchPath(const std::string& path);
	void RemoveSearchPath(int index);
	const std::vector<std::string>& GetSearchPaths() const { return mSearchPaths; }

	// recursive plugin scan, on a background thread
	//
	// NOTE: a call made while a scan is already running is dropped, not queued. every
	// call used to detach a thread of its own, so holding the settings button down put
	// several of them inside plugin entry points at the same time
	void ScanPlugins();

	bool IsScanning() const { return mScanning.load(std::memory_order_relaxed); }

	std::vector<PluginInfo> GetKnownPlugins() {
		std::lock_guard<std::mutex> lock(mMutex);
		return mPlugins;
	}

	// held around every load, instantiation and unload of a plugin binary, wherever it
	// happens from. a plugin's entry point does global initialization on the way in -
	// GL contexts, COM apartments, static registries - and none of that is written to
	// survive two threads arriving at once. the scan thread and the UI thread both
	// reach it, and opening a project while the startup scan is still running is the
	// usual way they meet
	static std::mutex& BinaryLock();
private:
	std::vector<std::string> mSearchPaths; // UI thread only; the scan copies it before starting
	std::vector<PluginInfo> mPlugins;
	std::mutex mMutex;

	std::thread mScanThread;
	std::atomic<bool> mScanning{false};
	std::atomic<bool> mAbortScan{false};
};
