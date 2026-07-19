#pragma once
#include <string>

// small app-wide configuration that persists across sessions (separate from the
// per-project file). Stored as a tiny key/value text file under %APPDATA%/MSDAW
class AppConfig {
public:
	static AppConfig& Instance();

	// when true, plugin editor windows are created DPI-aware so they render at the
	// display's native resolution (crisp). When false, windows are created
	// DPI-unaware and Windows bitmap-stretches them to match the DAW scale (matches
	// the DAW size but can look blurry / tear on fractional-DPI displays).
	// This is the global DEFAULT; individual plugins can override it (see
	// EditorScalingMode on AudioProcessor)
	bool pluginEditorsNative = true;

	void Load();
	void Save() const;
private:
	AppConfig() = default;
	std::string ConfigPath() const;
};
