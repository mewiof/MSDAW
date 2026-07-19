#include "PrecompHeader.h"
#include "AppConfig.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>

AppConfig& AppConfig::Instance() {
	static AppConfig instance;
	return instance;
}

std::string AppConfig::ConfigPath() const {
#ifdef _WIN32
	const char* appData = std::getenv("APPDATA");
	std::filesystem::path base = appData ? std::filesystem::path(appData) : std::filesystem::current_path();
	base /= "MSDAW";
#else
	const char* home = std::getenv("HOME");
	std::filesystem::path base = home ? std::filesystem::path(home) : std::filesystem::current_path();
	base /= ".config/MSDAW";
#endif
	return (base / "config.txt").string();
}

void AppConfig::Load() {
	std::ifstream in(ConfigPath());
	if (!in.is_open())
		return; // keep defaults

	std::string line;
	while (std::getline(in, line)) {
		std::stringstream ss(line);
		std::string key;
		ss >> key;
		if (key == "plugin_editors_native") {
			int v = 1;
			ss >> v;
			pluginEditorsNative = (v != 0);
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
}
