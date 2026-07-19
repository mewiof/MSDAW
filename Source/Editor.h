#pragma once
#include "AudioEngine.h"
#include "EditorContext.h"
#include "Views/TransportView.h"
#include "Views/LibraryView.h"
#include "Views/TrackListView.h"
#include "Views/TimelineView.h"
#include "Views/DeviceRackView.h"
#include "Views/ClipView.h"
#include "Views/PianoRollView.h"
#include <memory>
#include <string>

// forward declaration
struct ImVec2;

class Editor {
public:
	Editor(AudioEngine& engine);
	~Editor();

	// init ui scaling
	void Init(float scale);

	// set hwnd for VST
	void SetNativeWindowHandle(void* handle) { mContext.nativeWindowHandle = handle; }

	// main render loop
	void Render(const ImVec2& workPos, const ImVec2& workSize);

	// handle file drops
	void OnFileDrop(const std::string& path, float x, float y);

	// handle drag hover
	void OnDragOver(float x, float y);

	// reset drag flags
	void ClearDragState();

	// handle VST keys
	void OnExternalKey(int virtualKey, bool isDown);

	// project management
	void NewProject();
	void SaveProject();
	void SaveProjectAs();
	void OpenProject();
	void ExportProject();

	// transport logic
	void TogglePlayStop();
private:
	void RenderMenuBar();
	void RenderSettingsWindow();
	void ProcessComputerKeyboardMIDI(); // imgui input
	void HandleGlobalShortcuts();
	void PumpPluginEditors(); // per-frame idle for open plugin editor windows
	Project* GetProject();
private:
	EditorContext mContext;

	// sub-views
	std::unique_ptr<TransportView> mTransportView;
	std::unique_ptr<LibraryView> mLibraryView;
	std::unique_ptr<TrackListView> mTrackListView;
	std::unique_ptr<TimelineView> mTimelineView;
	std::unique_ptr<DeviceRackView> mDeviceRackView;
	std::unique_ptr<ClipView> mClipView;
	std::unique_ptr<PianoRollView> mPianoRollView;

	// state
	std::string mCurrentProjectPath;
	int mActiveBottomTab = 0; // 0 - devices, 1 - clips
};
