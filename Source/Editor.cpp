#include "PrecompHeader.h"
#include "Editor.h"
#include "AppConfig.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <cmath>

#ifdef _WIN32
#include <windows.h> // VK_ codes
#include <commdlg.h> // win32 dialogs
#endif

Editor::Editor(AudioEngine& engine)
	: mContext(engine) {
	mTransportView = std::make_unique<TransportView>(mContext);
	mLibraryView = std::make_unique<LibraryView>(mContext);
	mTrackListView = std::make_unique<TrackListView>(mContext);
	mTimelineView = std::make_unique<TimelineView>(mContext);
	mDeviceRackView = std::make_unique<DeviceRackView>(mContext);
	mClipView = std::make_unique<ClipView>(mContext);
	mPianoRollView = std::make_unique<PianoRollView>(mContext);

	VSTProcessor::OnGlobalKeyEvent = [this](int virtualKey, bool isDown) {
		this->OnExternalKey(virtualKey, isDown);
	};
}

Editor::~Editor() {
}

void Editor::Init(float scale) {
	mContext.state.mainScale = scale;
	mContext.layout.Scale(scale);
	mContext.state.pixelsPerBeat *= scale;

	// scan for VSTs on startup
	mContext.pluginManager.ScanPlugins();
}

Project* Editor::GetProject() {
	return mContext.GetProject();
}

void Editor::OnFileDrop(const std::string& path, float x, float y) {
	mContext.state.droppedPath = path;
	mContext.state.dropX = x;
	mContext.state.dropY = y;
	mContext.state.processDrop = true;
	mContext.state.isOsDragging = false;
}

void Editor::OnDragOver(float x, float y) {
	mContext.state.isOsDragging = true;
	mContext.state.osDragX = x;
	mContext.state.osDragY = y;
	mContext.state.lastOsDragTime = ImGui::GetTime();
}

void Editor::ClearDragState() {
	// logic moved to render
}

void Editor::NewProject() {
	if (Project* p = GetProject()) {
		p->Initialize();
		mCurrentProjectPath.clear();
		mContext.state.selectionStart = 0.0;
		mContext.state.selectionEnd = 0.0;
		mContext.state.pixelsPerBeat = 60.0f;
		mContext.state.timelineScrollX = 0.0f;
		mContext.state.timelineScrollY = 0.0f;
		mContext.state.restoreScroll = true;

		mContext.state.timelineGridNumerator = 1;
		mContext.state.timelineGridDenominator = 4;
		mContext.state.timelineGrid = 0.25;
	}
}

void Editor::SaveProject() {
	if (mCurrentProjectPath.empty()) {
		SaveProjectAs();
	} else {
		if (Project* p = GetProject()) {
			ProjectViewState vs;
			vs.pixelsPerBeat = mContext.state.pixelsPerBeat;
			vs.selectionStart = mContext.state.selectionStart;
			vs.selectionEnd = mContext.state.selectionEnd;
			vs.scrollX = mContext.state.timelineScrollX;
			vs.scrollY = mContext.state.timelineScrollY;
			vs.timelineGridNumerator = mContext.state.timelineGridNumerator;
			vs.timelineGridDenominator = mContext.state.timelineGridDenominator;
			p->SetViewState(vs);
			p->Save(mCurrentProjectPath);
		}
	}
}

void Editor::SaveProjectAs() {
#ifdef _WIN32
	OPENFILENAMEA ofn;
	char szFile[260] = {0};

	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = (HWND)mContext.nativeWindowHandle;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = "MSDAW Project\0*.msdaw\0All Files\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
	ofn.lpstrDefExt = "msdaw";

	if (GetSaveFileNameA(&ofn) == TRUE) {
		mCurrentProjectPath = szFile;
		if (Project* p = GetProject()) {
			ProjectViewState vs;
			vs.pixelsPerBeat = mContext.state.pixelsPerBeat;
			vs.selectionStart = mContext.state.selectionStart;
			vs.selectionEnd = mContext.state.selectionEnd;
			vs.scrollX = mContext.state.timelineScrollX;
			vs.scrollY = mContext.state.timelineScrollY;
			vs.timelineGridNumerator = mContext.state.timelineGridNumerator;
			vs.timelineGridDenominator = mContext.state.timelineGridDenominator;
			p->SetViewState(vs);
			p->Save(mCurrentProjectPath);
		}
	}
#endif
}

void Editor::OpenProject() {
#ifdef _WIN32
	OPENFILENAMEA ofn;
	char szFile[260] = {0};

	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = (HWND)mContext.nativeWindowHandle;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = "MSDAW Project\0*.msdaw\0All Files\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

	if (GetOpenFileNameA(&ofn) == TRUE) {
		mCurrentProjectPath = szFile;
		if (Project* p = GetProject()) {
			p->Load(mCurrentProjectPath);

			const auto& vs = p->GetViewState();
			mContext.state.pixelsPerBeat = std::max(10.0f, vs.pixelsPerBeat);
			mContext.state.selectionStart = vs.selectionStart;
			mContext.state.selectionEnd = vs.selectionEnd;
			mContext.state.timelineScrollX = vs.scrollX;
			mContext.state.timelineScrollY = vs.scrollY;

			mContext.state.timelineGridNumerator = vs.timelineGridNumerator;
			mContext.state.timelineGridDenominator = vs.timelineGridDenominator;
			if (mContext.state.timelineGridDenominator <= 0)
				mContext.state.timelineGridDenominator = 4;
			mContext.state.timelineGrid = (double)mContext.state.timelineGridNumerator / mContext.state.timelineGridDenominator;

			mContext.state.restoreScroll = true;
		}
	}
#endif
}

void Editor::ExportProject() {
#ifdef _WIN32
	OPENFILENAMEA ofn;
	char szFile[260] = {0};

	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = (HWND)mContext.nativeWindowHandle;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = "WAV File\0*.wav\0All Files\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
	ofn.lpstrDefExt = "wav";

	if (GetSaveFileNameA(&ofn) == TRUE) {
		if (Project* p = GetProject()) {
			double start = 0.0;
			double end = 0.0;
			if (mContext.state.selectionEnd > mContext.state.selectionStart) {
				start = mContext.state.selectionStart;
				end = mContext.state.selectionEnd;
			}

			if (p->RenderAudio(szFile, start, end)) {
				MessageBoxA((HWND)mContext.nativeWindowHandle, "Export Complete!", "Success", MB_OK);
			} else {
				MessageBoxA((HWND)mContext.nativeWindowHandle, "Export Failed.", "Error", MB_OK | MB_ICONERROR);
			}
		}
	}
#endif
}

void Editor::TogglePlayStop() {
	if (Project* project = GetProject()) {
		Transport& transport = project->GetTransport();
		if (transport.IsPlaying()) {
			transport.Pause();
			double startBeat = mContext.state.selectionStart;
			int64_t startSample = (int64_t)(startBeat * (60.0 / transport.GetBpm()) * transport.GetSampleRate());
			transport.SetPosition(startSample);
		} else {
			double startBeat = mContext.state.selectionStart;
			int64_t startSample = (int64_t)(startBeat * (60.0 / transport.GetBpm()) * transport.GetSampleRate());
			transport.SetPosition(startSample);
			transport.Play();
		}
	}
}

void Editor::OnExternalKey(int virtualKey, bool isDown) {
#ifdef _WIN32
	if (!isDown)
		return;
	// TODO: remove this?
#endif
}

void Editor::HandleGlobalShortcuts() {
#ifdef _WIN32
	DWORD foregroundPid = 0;
	HWND hWndFG = GetForegroundWindow();
	if (hWndFG)
		GetWindowThreadProcessId(hWndFG, &foregroundPid);

	bool isOurProcessForeground = (foregroundPid == GetCurrentProcessId());
	bool isMainWindowForeground = (hWndFG == (HWND)mContext.nativeWindowHandle);

	if (isMainWindowForeground && ImGui::GetIO().WantTextInput)
		return;

	if (!isOurProcessForeground)
		return;

	HWND hFocus = GetFocus();
	if (hFocus && hFocus != (HWND)mContext.nativeWindowHandle) {
		char className[256] = {0};
		GetClassNameA(hFocus, className, sizeof(className));
		std::string cls = className;
		std::transform(cls.begin(), cls.end(), cls.begin(), ::tolower);
		// exempt our plugin wrapper classes from the text-input heuristic
		if (cls != "vsteditorclass" && cls != "vst3editorclass" && cls.find("edit") != std::string::npos) {
			return;
		}
	}

	static bool spaceWasDown = false;
	bool spaceIsDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
	if (spaceIsDown && !spaceWasDown) {
		TogglePlayStop();
	}
	spaceWasDown = spaceIsDown;

	bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

	static bool sWasDown = false;
	bool sIsDown = (GetAsyncKeyState('S') & 0x8000) != 0;
	if (ctrl && sIsDown && !sWasDown) {
		SaveProject();
	}
	sWasDown = sIsDown;

	static bool gWasDown = false;
	bool gIsDown = (GetAsyncKeyState('G') & 0x8000) != 0;
	if (ctrl && gIsDown && !gWasDown) {
		if (Project* p = GetProject()) {
			if (!mContext.state.multiSelectedTracks.empty()) {
				p->GroupSelectedTracks(mContext.state.multiSelectedTracks);
				mContext.state.multiSelectedTracks.clear();
			}
		}
	}
	gWasDown = gIsDown;

#else
	// focus states where possible
	if (ImGui::GetIO().WantTextInput)
		return;

	bool ctrl = ImGui::GetIO().KeyCtrl;

	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
		SaveProject();
	}

	if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
		TogglePlayStop();
	}

	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
		if (Project* p = GetProject()) {
			if (!mContext.state.multiSelectedTracks.empty()) {
				p->GroupSelectedTracks(mContext.state.multiSelectedTracks);
				mContext.state.multiSelectedTracks.clear();
			}
		}
	}
#endif
}

void Editor::RenderMenuBar() {
	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("New Project"))
				NewProject();
			if (ImGui::MenuItem("Open Project..."))
				OpenProject();
			if (ImGui::MenuItem("Save Project", "Ctrl+S"))
				SaveProject();
			if (ImGui::MenuItem("Save Project As..."))
				SaveProjectAs();
			ImGui::Separator();
			if (ImGui::MenuItem("Export Audio..."))
				ExportProject();
			ImGui::Separator();
			if (ImGui::MenuItem("Exit"))
				exit(0);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Windows")) {
			if (ImGui::MenuItem("Settings", nullptr, mContext.state.showSettingsWindow)) {
				mContext.state.showSettingsWindow = !mContext.state.showSettingsWindow;
			}
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}
}

void Editor::ProcessComputerKeyboardMIDI() {
#ifdef _WIN32
	// only process raw MIDI if main window is focused
	DWORD foregroundPid = 0;
	HWND hWndFG = GetForegroundWindow();
	if (hWndFG)
		GetWindowThreadProcessId(hWndFG, &foregroundPid);

	bool isOurProcessForeground = (foregroundPid == GetCurrentProcessId());
	bool isMainWindowForeground = (hWndFG == (HWND)mContext.nativeWindowHandle);

	if (isMainWindowForeground && ImGui::GetIO().WantTextInput)
		return;

	if (!isOurProcessForeground) {
		// if not focused, release all notes
		if (!mContext.state.activeMIDINotes.empty()) {
			for (int note : mContext.state.activeMIDINotes)
				mContext.engine.SendMIDIEvent(0x80, note, 0);
			mContext.state.activeMIDINotes.clear();
		}
		return;
	}

	HWND hFocus = GetFocus();
	if (hFocus && hFocus != (HWND)mContext.nativeWindowHandle) {
		char className[256] = {0};
		GetClassNameA(hFocus, className, sizeof(className));
		std::string cls = className;
		std::transform(cls.begin(), cls.end(), cls.begin(), ::tolower);
		// exempt our plugin wrapper classes from the text-input heuristic
		if (cls != "vsteditorclass" && cls != "vst3editorclass" && cls.find("edit") != std::string::npos) {
			return;
		}
	}

	static bool mWasDown = false;
	bool mIsDown = (GetAsyncKeyState('M') & 0x8000) != 0;
	if (mIsDown && !mWasDown)
		mContext.state.isComputerMIDIKeyboardEnabled = !mContext.state.isComputerMIDIKeyboardEnabled;
	mWasDown = mIsDown;

	if (!mContext.state.isComputerMIDIKeyboardEnabled) {
		if (!mContext.state.activeMIDINotes.empty()) {
			for (int note : mContext.state.activeMIDINotes)
				mContext.engine.SendMIDIEvent(0x80, note, 0);
			mContext.state.activeMIDINotes.clear();
		}
		return;
	}

	static bool zWasDown = false;
	if ((GetAsyncKeyState('Z') & 0x8000) && !zWasDown)
		mContext.state.mIDIOctave = std::max(0, mContext.state.mIDIOctave - 1);
	zWasDown = (GetAsyncKeyState('Z') & 0x8000) != 0;

	static bool xWasDown = false;
	if ((GetAsyncKeyState('X') & 0x8000) && !xWasDown)
		mContext.state.mIDIOctave = std::min(8, mContext.state.mIDIOctave + 1);
	xWasDown = (GetAsyncKeyState('X') & 0x8000) != 0;

	static bool cWasDown = false;
	if ((GetAsyncKeyState('C') & 0x8000) && !cWasDown)
		mContext.state.mIDIVelocity = std::max(1, mContext.state.mIDIVelocity - 20);
	cWasDown = (GetAsyncKeyState('C') & 0x8000) != 0;

	static bool vWasDown = false;
	if ((GetAsyncKeyState('V') & 0x8000) && !vWasDown)
		mContext.state.mIDIVelocity = std::min(127, mContext.state.mIDIVelocity + 20);
	vWasDown = (GetAsyncKeyState('V') & 0x8000) != 0;

	int baseNote = (mContext.state.mIDIOctave + 1) * 12;

	struct KeyMap {
		int vk;
		int semitoneOffset;
	};
	static const std::vector<KeyMap> keyMappings = {
		{'A', 0}, {'W', 1}, {'S', 2}, {'E', 3}, {'D', 4}, {'F', 5}, {'T', 6}, {'G', 7}, {'Y', 8}, {'H', 9}, {'U', 10}, {'J', 11}, {'K', 12}, {'O', 13}, {'L', 14}, {'P', 15}};

	for (const auto& mapping : keyMappings) {
		int mIDINote = baseNote + mapping.semitoneOffset;
		if (mIDINote > 127)
			continue;

		// skip if mod is down
		if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
			continue;

		bool isDown = (GetAsyncKeyState(mapping.vk) & 0x8000) != 0;
		bool wasDown = mContext.state.activeMIDINotes.count(mIDINote) > 0;

		if (isDown && !wasDown) {
			mContext.engine.SendMIDIEvent(0x90, mIDINote, mContext.state.mIDIVelocity);
			mContext.state.activeMIDINotes.insert(mIDINote);
		} else if (!isDown && wasDown) {
			mContext.engine.SendMIDIEvent(0x80, mIDINote, 0);
			mContext.state.activeMIDINotes.erase(mIDINote);
		}
	}
#else
	// cross-platform logic
	if (ImGui::GetIO().WantTextInput)
		return;

	if (ImGui::IsKeyPressed(ImGuiKey_M, false))
		mContext.state.isComputerMIDIKeyboardEnabled = !mContext.state.isComputerMIDIKeyboardEnabled;

	if (!mContext.state.isComputerMIDIKeyboardEnabled) {
		if (!mContext.state.activeMIDINotes.empty()) {
			for (int note : mContext.state.activeMIDINotes)
				mContext.engine.SendMIDIEvent(0x80, note, 0);
			mContext.state.activeMIDINotes.clear();
		}
		return;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
		mContext.state.mIDIOctave = std::max(0, mContext.state.mIDIOctave - 1);
	if (ImGui::IsKeyPressed(ImGuiKey_X, false))
		mContext.state.mIDIOctave = std::min(8, mContext.state.mIDIOctave + 1);
	if (ImGui::IsKeyPressed(ImGuiKey_C, false))
		mContext.state.mIDIVelocity = std::max(1, mContext.state.mIDIVelocity - 20);
	if (ImGui::IsKeyPressed(ImGuiKey_V, false))
		mContext.state.mIDIVelocity = std::min(127, mContext.state.mIDIVelocity + 20);
	int baseNote = (mContext.state.mIDIOctave + 1) * 12;
	struct KeyMapImGui {
		ImGuiKey key;
		int semitoneOffset;
	};
	static const std::vector<KeyMapImGui> keyMappings = {
		{ImGuiKey_A, 0}, {ImGuiKey_W, 1}, {ImGuiKey_S, 2}, {ImGuiKey_E, 3}, {ImGuiKey_D, 4}, {ImGuiKey_F, 5}, {ImGuiKey_T, 6}, {ImGuiKey_G, 7}, {ImGuiKey_Y, 8}, {ImGuiKey_H, 9}, {ImGuiKey_U, 10}, {ImGuiKey_J, 11}, {ImGuiKey_K, 12}, {ImGuiKey_O, 13}, {ImGuiKey_L, 14}, {ImGuiKey_P, 15}};
	for (const auto& mapping : keyMappings) {
		int mIDINote = baseNote + mapping.semitoneOffset;
		if (mIDINote > 127)
			continue;

		// skip if mod is down
		if (ImGui::GetIO().KeyCtrl)
			continue;

		if (ImGui::IsKeyPressed(mapping.key, false)) {
			mContext.engine.SendMIDIEvent(0x90, mIDINote, mContext.state.mIDIVelocity);
			mContext.state.activeMIDINotes.insert(mIDINote);
		}
		if (ImGui::IsKeyReleased(mapping.key)) {
			mContext.engine.SendMIDIEvent(0x80, mIDINote, 0);
			mContext.state.activeMIDINotes.erase(mIDINote);
		}
	}
#endif
}

void Editor::PumpPluginEditors() {
	// give every open plugin editor a chance to service its GUI each frame. VST2
	// plugins need effEditIdle to repaint smoothly; other processors no-op
	Project* project = GetProject();
	if (!project)
		return;

	if (auto master = project->GetMasterTrack()) {
		for (auto& proc : master->GetProcessors()) {
			if (proc && proc->IsEditorOpen())
				proc->EditorIdle();
		}
	}
	for (auto& track : project->GetTracks()) {
		if (!track)
			continue;
		for (auto& proc : track->GetProcessors()) {
			if (proc && proc->IsEditorOpen())
				proc->EditorIdle();
		}
	}
}

void Editor::RenderSettingsWindow() {
	if (!mContext.state.showSettingsWindow)
		return;

	ImGui::ShowDemoWindow();

	if (ImGui::Begin("Settings", &mContext.state.showSettingsWindow, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (ImGui::BeginTabBar("SettingsTabs")) {
			if (ImGui::BeginTabItem("Audio")) {
				ImGui::Text("Device: Default Output");
				ImGui::Text("Sample Rate: 48000 Hz");
				ImGui::Text("Buffer Size: 512");
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Display & Input")) {
				ImGui::Text("Scrolling Behavior");
				if (ImGui::RadioButton("Page", mContext.state.followMode == FollowMode::Page))
					mContext.state.followMode = FollowMode::Page;
				ImGui::SameLine();
				if (ImGui::RadioButton("Continuous", mContext.state.followMode == FollowMode::Continuous))
					mContext.state.followMode = FollowMode::Continuous;
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Plugins")) {
				ImGui::TextUnformatted("Plugin Editor Windows");
				bool native = AppConfig::Instance().pluginEditorsNative;
				if (ImGui::Checkbox("Render at native resolution (crisp, DPI-aware)", &native)) {
					AppConfig::Instance().pluginEditorsNative = native;
					AppConfig::Instance().Save();
				}
				ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
								   "Off = let Windows scale plugin windows to match the DAW (may blur).\n"
								   "Right-click a device in the rack to override this per plugin.");
				ImGui::Separator();

				ImGui::Text("VST2/VST3 Search Paths:");
				ImGui::Separator();
				PluginManager& pm = mContext.pluginManager;
				std::vector<std::string> paths = pm.GetSearchPaths();
				int pathToRemove = -1;
				ImGui::BeginChild("PathList", ImVec2(400, 150), true);
				for (int i = 0; i < (int)paths.size(); ++i) {
					ImGui::Text("%s", paths[i].c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton(("X##" + std::to_string(i)).c_str()))
						pathToRemove = i;
				}
				ImGui::EndChild();
				if (pathToRemove != -1)
					pm.RemoveSearchPath(pathToRemove);
				static char newPathBuf[256] = "";
				ImGui::InputText("##NewPath", newPathBuf, 256);
				ImGui::SameLine();
				if (ImGui::Button("Add Path")) {
					if (strlen(newPathBuf) > 0) {
						pm.AddSearchPath(newPathBuf);
						memset(newPathBuf, 0, 256);
					}
				}
				ImGui::Separator();
				if (ImGui::Button("Scan Plugins", ImVec2(120, 30)))
					pm.ScanPlugins();
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Found: %d", (int)pm.GetKnownPlugins().size());
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}

void Editor::Render(const ImVec2& fullWorkPos, const ImVec2& fullWorkSize) {
	// check for drag timeout
	if (mContext.state.isOsDragging) {
		if (ImGui::GetTime() - mContext.state.lastOsDragTime > 0.2) {
			mContext.state.isOsDragging = false;
		}
	}

	if (Project* p = GetProject()) {
		p->SetSelectedTrack(mContext.state.selectedTrackIndex);
	}

	HandleGlobalShortcuts();
	ProcessComputerKeyboardMIDI();
	RenderMenuBar();

	ImVec2 workPos = ImVec2(fullWorkPos.x, fullWorkPos.y);
	ImVec2 workSize = ImVec2(fullWorkSize.x, fullWorkSize.y);

	float transportH = mContext.layout.transportHeight;
	float bottomH = mContext.layout.bottomPanelHeight;
	float libraryW = mContext.layout.libraryWidth;
	float trackListW = mContext.layout.trackListWidth;
	float middleHeight = workSize.y - transportH - bottomH;

	mTransportView->Render(workPos, workSize.x, transportH);
	mLibraryView->Render(ImVec2(workPos.x, workPos.y + transportH), libraryW, middleHeight);
	mTimelineView->Render(ImVec2(workPos.x + libraryW, workPos.y + transportH), workSize.x - libraryW, middleHeight, mTrackListView.get(), trackListW);

	// bottom tab panel
	float tabHeight = 28.0f * mContext.state.mainScale;

	ImGui::SetNextWindowPos(ImVec2(workPos.x, workPos.y + workSize.y - bottomH));
	ImGui::SetNextWindowSize(ImVec2(workSize.x, tabHeight));
	ImGui::Begin("BottomTabs", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);

	if (ImGui::BeginTabBar("MainTabs")) {
		if (ImGui::BeginTabItem("Device View")) {
			mActiveBottomTab = 0;
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Clip View")) {
			mActiveBottomTab = 1;
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::End();

	float contentY = workPos.y + workSize.y - bottomH + tabHeight;
	float contentH = bottomH - tabHeight;

	if (mActiveBottomTab == 0) {
		mDeviceRackView->Render(ImVec2(workPos.x, contentY), workSize.x, contentH);
	} else {
		mClipView->Render(ImVec2(workPos.x, contentY), workSize.x, contentH);
	}

	mPianoRollView->Render();
	RenderSettingsWindow();
	PumpPluginEditors();

	if (mContext.state.processDrop) {
		std::filesystem::path p(mContext.state.droppedPath);
		std::string ext = p.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		if (ext == ".dll") {
			Project* project = GetProject();
			if (project) {
				auto vST = std::make_shared<VSTProcessor>(mContext.state.droppedPath);
				if (vST->Load()) {
					project->CreateTrack();
					auto& tracks = project->GetTracks();
					if (!tracks.empty()) {
						auto track = tracks.back();
						track->SetName(p.filename().stem().string());
						track->AddProcessor(vST);
						if (project->GetTransport().GetSampleRate() > 0)
							vST->PrepareToPlay(project->GetTransport().GetSampleRate());
					}
				}
			}
		} else if (ext == ".vst3") {
			Project* project = GetProject();
			if (project) {
				auto vST = std::make_shared<VST3Processor>(mContext.state.droppedPath);
				if (vST->Load()) {
					project->CreateTrack();
					auto& tracks = project->GetTracks();
					if (!tracks.empty()) {
						auto track = tracks.back();
						track->SetName(p.filename().stem().string());
						track->AddProcessor(vST);
						if (project->GetTransport().GetSampleRate() > 0)
							vST->PrepareToPlay(project->GetTransport().GetSampleRate());
					}
				}
			}
		}
		mContext.state.processDrop = false;
	}

	Parameter* requestedParameter = Parameter::GetAndClearAutomationRequestParameter();
	if (requestedParameter)
		if (Project* p = GetProject()) {
			bool found = false;
			for (size_t i = 0; i < p->GetTracks().size(); ++i) {
				auto track = p->GetTracks()[i];
				auto params = track->GetAllParameters();
				if (std::find(params.begin(), params.end(), requestedParameter) != params.end()) {
					track->mShowAutomation = true;
					track->mSelectedAutomationParam = requestedParameter;
					mContext.state.selectedTrackIndex = (int)i;
					found = true;
					break;
				}
			}
			if (!found) {
				auto master = p->GetMasterTrack();
				if (master) {
					auto params = master->GetAllParameters();
					if (std::find(params.begin(), params.end(), requestedParameter) != params.end()) {
						master->mShowAutomation = true;
						master->mSelectedAutomationParam = requestedParameter;
						mContext.state.selectedTrackIndex = -1;
					}
				}
			}
		}
}
