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
	// the DAW size but can look blurry / tear on fractional-DPI displays)
	// this is the global DEFAULT; individual plugins can override it (see
	// EditorScalingMode on AudioProcessor)
	bool pluginEditorsNative = true;

	// panels the user has folded away. app-wide rather than per project: a screen with
	// no room for the library has no room for it in the next project either
	bool libraryCollapsed = false;
	bool bottomPanelCollapsed = false;

	void Load();
	void Save() const;

	// the app's own folder (%APPDATA%/MSDAW, ~/.config/MSDAW), created on demand by
	// whoever writes into it. the fallback home for files the app generates when the
	// project they belong to has never been saved anywhere
	static std::string DataDirectory();
private:
	AppConfig() = default;
	std::string ConfigPath() const;
};
