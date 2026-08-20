#include "PrecompHeader.h"
#include "PluginManager.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"
#include <filesystem>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

std::mutex& PluginManager::BinaryLock() {
	static std::mutex lock;
	return lock;
}

PluginManager::PluginManager() {
	// default paths
	mSearchPaths.push_back("C:\\Program Files\\VSTPlugins");
	mSearchPaths.push_back("C:\\Program Files (x86)\\VSTPlugins");
	mSearchPaths.push_back("C:\\Program Files\\Steinberg\\VSTPlugins");
	mSearchPaths.push_back("C:\\Program Files\\Common Files\\VST3");
}

PluginManager::~PluginManager() {
	// the scan writes mPlugins through `this`. a detached thread outliving the manager
	// is a use-after-free that lands on whatever gets built where it used to be
	mAbortScan.store(true, std::memory_order_relaxed);
	if (mScanThread.joinable())
		mScanThread.join();
}

void PluginManager::AddSearchPath(const std::string& path) {
	// avoid duplicates
	for (const auto& p : mSearchPaths) {
		if (p == path)
			return;
	}
	mSearchPaths.push_back(path);
}

void PluginManager::RemoveSearchPath(int index) {
	if (index >= 0 && index < (int)mSearchPaths.size()) {
		mSearchPaths.erase(mSearchPaths.begin() + index);
	}
}

void PluginManager::ScanPlugins() {
	// one scan at a time. the flag is claimed here, on the calling thread, so two
	// clicks in the same frame cannot both get through
	bool idle = false;
	if (!mScanning.compare_exchange_strong(idle, true, std::memory_order_acq_rel))
		return;

	// the previous scan has finished its work - that is what cleared the flag - but the
	// thread object still owns a handle, and assigning over a joinable one terminates
	if (mScanThread.joinable())
		mScanThread.join();

	// mSearchPaths belongs to the UI thread, which is the only caller here
	const std::vector<std::string> pathsToScan = mSearchPaths;
	mAbortScan.store(false, std::memory_order_relaxed);

	mScanThread = std::thread([this, pathsToScan]() {
		std::cout << "Scanning for Plugins in background...\n";
		std::vector<PluginInfo> foundPlugins;

		for (const auto& pathStr : pathsToScan) {
			if (mAbortScan.load(std::memory_order_relaxed))
				break;
			fs::path root(pathStr);
			if (!fs::exists(root) || !fs::is_directory(root))
				continue;

			try {
				for (const auto& entry : fs::recursive_directory_iterator(root)) {
					// quitting must not wait out a full sweep of every VST folder
					if (mAbortScan.load(std::memory_order_relaxed))
						break;
					if (entry.is_regular_file()) {
						std::string ext = entry.path().extension().string();
						std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

						if (ext == ".dll") {
							std::string fullPath = entry.path().string();

							// metadata scan
							VSTProcessor tempProc(fullPath);
							if (tempProc.Load()) {
								PluginInfo info;
								info.name = tempProc.GetName();
								info.path = fullPath;
								info.isSynth = tempProc.IsInstrument();
								info.vendor = ""; // VST2 metadata limitation
								info.format = "VST2";

								foundPlugins.push_back(info);
								std::cout << "Found: " << info.name << " (" << (info.isSynth ? "Inst" : "FX") << ") [VST2]\n";
							}
						} else if (ext == ".vst3") {
							std::string fullPath = entry.path().string();
							auto vst3Infos = VST3Processor::EnumeratePlugins(fullPath);
							for (auto& info : vst3Infos) {
								foundPlugins.push_back(info);
								std::cout << "Found: " << info.name << " (" << (info.isSynth ? "Inst" : "FX") << ") [VST3]\n";
							}
						}
					}
				}
			} catch (const std::exception& e) {
				std::cout << "Error scanning directory " << pathStr << ": " << e.what() << "\n";
			}
		}

		// an aborted scan only saw part of the tree, so it must not replace the list the
		// UI is already showing with a truncated one
		if (!mAbortScan.load(std::memory_order_relaxed)) {
			std::lock_guard<std::mutex> lock(mMutex);
			mPlugins = std::move(foundPlugins);
		}
		std::cout << "Scan Complete. Found plugins.\n";
		// last thing the thread does: clearing this lets the next scan start, and the
		// join above is what waits for the thread itself to finish
		mScanning.store(false, std::memory_order_release);
	});
}
