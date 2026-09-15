#include "PrecompHeader.h"
#include "AppConfig.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>

AppConfig& AppConfig::Instance() {
	static AppConfig instance;
	return instance;
}

std::string AppConfig::DataDirectory() {
#ifdef _WIN32
	const char* appData = std::getenv("APPDATA");
	std::filesystem::path base = appData ? std::filesystem::path(appData) : std::filesystem::current_path();
	base /= "MSDAW";
#else
	const char* home = std::getenv("HOME");
	std::filesystem::path base = home ? std::filesystem::path(home) : std::filesystem::current_path();
	base /= ".config/MSDAW";
#endif
	return base.string();
}

std::string AppConfig::ConfigPath() const {
	return (std::filesystem::path(DataDirectory()) / "config.txt").string();
}

void AppConfig::Load() {
	std::ifstream in(ConfigPath());
	if (!in.is_open())
		return; // keep defaults

	// the folder list is the one accumulating key here, so a second Load would stack
	// the file's folders on top of the ones already read
	libraryFolders.clear();

	std::string line;
	while (std::getline(in, line)) {
		std::stringstream ss(line);
		std::string key;
		ss >> key;
		if (key == "plugin_editors_native") {
			int v = 1;
			ss >> v;
			pluginEditorsNative = (v != 0);
		} else if (key == "library_collapsed") {
			int v = 0;
			ss >> v;
			libraryCollapsed = (v != 0);
		} else if (key == "bottom_panel_collapsed") {
			int v = 0;
			ss >> v;
			bottomPanelCollapsed = (v != 0);
		} else if (key == "library_folder") {
			// the rest of the line, not the next token: a folder path has spaces in it
			// more often than not
			std::string folder;
			std::getline(ss, folder);
			size_t firstNonSpace = folder.find_first_not_of(' ');
			if (firstNonSpace != std::string::npos)
				libraryFolders.push_back(folder.substr(firstNonSpace));
		} else if (key == "library_preview") {
			int v = 1;
			ss >> v;
			libraryPreview = (v != 0);
		} else if (key == "library_devices_split") {
			float v = 0.4f;
			ss >> v;
			libraryDevicesSplit = std::clamp(v, 0.1f, 0.9f);
		} else if (key == "library_browser_split") {
			float v = 0.5f;
			ss >> v;
			libraryBrowserSplit = std::clamp(v, 0.1f, 0.9f);
		}
	}
}

void AppConfig::Save() const {
	std::filesystem::path path(ConfigPath());
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);

	std::ofstream out(path);
	if (!out.is_open())
		return;

	out << "plugin_editors_native " << (pluginEditorsNative ? 1 : 0) << "\n";
	out << "library_collapsed " << (libraryCollapsed ? 1 : 0) << "\n";
	out << "bottom_panel_collapsed " << (bottomPanelCollapsed ? 1 : 0) << "\n";
	out << "library_preview " << (libraryPreview ? 1 : 0) << "\n";
	out << "library_devices_split " << libraryDevicesSplit << "\n";
	out << "library_browser_split " << libraryBrowserSplit << "\n";
	// last, and one line each: a folder path is written raw, so it is the only key
	// here that can hold whitespace and the reader takes the rest of the line
	for (const auto& folder : libraryFolders)
		out << "library_folder " << folder << "\n";
}
