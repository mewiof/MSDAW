#pragma once
#include <vector>
#include <string>
#include <mutex>

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

	// add VST search path
	void AddSearchPath(const std::string& path);
	void RemoveSearchPath(int index);
	const std::vector<std::string>& GetSearchPaths() const { return mSearchPaths; }

	// recursive plugin scan
	void ScanPlugins();

	std::vector<PluginInfo> GetKnownPlugins() {
		std::lock_guard<std::mutex> lock(mMutex);
		return mPlugins;
	}
private:
	std::vector<std::string> mSearchPaths;
	std::vector<PluginInfo> mPlugins;
	std::mutex mMutex;
};
