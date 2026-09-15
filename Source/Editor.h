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
#include "SystemMonitor.h"
#include <chrono>
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

	// load a project straight from a path, with no dialog: the library explorer opens
	// one by double-click, and OpenProject itself lands here once a file is picked
	void OpenProjectFile(const std::string& path);
	void ExportProject();

	// transport logic
	void TogglePlayStop();
	// (re)start at the insert marker whether or not the transport is already running
	void PlayFromMarker();

	// undo/redo
	void PerformUndo();
	void PerformRedo();
private:
	// move the transport to the insert marker. beats are the authoring unit and samples
	// the playback one, so every seek the editor asks for converts through here
	void SeekToMarker();

	void RenderMenuBar();
	void RenderResourceMeter(); // cpu/ram readout pinned to the top-right of the menu bar
	void DrawMeterCell(const char* id, const char* label, float fraction, float heat, const char* valueText, const char* tooltip);
	void RenderSettingsWindow();
	void RenderHistoryWindow();
	void RenderThirdPartyWindow(); // what MSDAW is built on, with the license each part ships under
	void ProcessComputerKeyboardMIDI(); // imgui input
	void HandleGlobalShortcuts();
	void PumpPluginEditors(); // idle tick for open plugin editor windows, held to kEditorIdleHz
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

	// when the open plugin editors were last idled, so the rate they are serviced at
	// stays independent of how fast we draw our own frames (see PumpPluginEditors)
	std::chrono::steady_clock::time_point mLastEditorIdle{};

	// state
	// NOTE: the project's path on disk lives on EditorState, not here - the track list
	// needs it to put a bounced track's wav beside the project
	int mActiveBottomTab = 0; // 0 - devices, 1 - clips

	// which dependency the third-party window is showing the license of
	int mSelectedNotice = 0;

	// live cpu/ram sampling for the menu-bar resource meter
	SystemMonitor mSystemMonitor;
};
