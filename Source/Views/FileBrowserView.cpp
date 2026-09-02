#include "PrecompHeader.h"
#include "FileBrowserView.h"

#include "AppConfig.h"
#include "Library/LibraryImport.h"
#include "PathText.h"
#include "Project.h"
#include "Theme.h"
#include "Undo/Actions.h"

#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h> // SHBrowseForFolder
#endif

namespace {

	// a root is shown by its own folder name, but a drive ("C:\") has none - fall
	// back to the path itself rather than drawing an empty row
	std::string RootLabel(const std::string& path) {
		std::string name = PathText::FromPath(PathText::ToPath(path).filename());
		return name.empty() ? path : name;
	}

	// a MIDI file reads cool, the way an instrument does in the plugin list above;
	// everything else is ordinary text. the accent stays reserved for selection
	ImU32 KindColor(FileKind kind) {
		const Theme& th = Theme::Instance();
		switch (kind) {
			case FileKind::MIDI:
				return th.graphCurveCool;
			case FileKind::Project:
				return th.textMuted;
			default:
				return th.text;
		}
	}

	void ShowInFileManager(const std::string& path, bool isFolder) {
#ifdef _WIN32
		// the wide entry point, because the path is UTF-8 and the ANSI one would take
		// it as code-page bytes - a folder with a non-Latin name would not open
		const std::wstring wide = PathText::ToPath(path).wstring();
		if (isFolder) {
			ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		} else {
			// /select, opens the containing folder with the file already highlighted
			const std::wstring arguments = L"/select,\"" + wide + L"\"";
			ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
		}
#else
		(void)path;
		(void)isFolder;
#endif
	}

#ifdef _WIN32
	// the platform's own folder picker. false when the user cancelled
	bool PickFolder(void* ownerWindow, std::string& outPath) {
		wchar_t selected[MAX_PATH] = {0};

		BROWSEINFOW info;
		ZeroMemory(&info, sizeof(info));
		info.hwndOwner = (HWND)ownerWindow;
		info.pszDisplayName = selected;
		info.lpszTitle = L"Add a folder to the library";
		info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

		LPITEMIDLIST picked = SHBrowseForFolderW(&info);
		if (!picked)
			return false;

		const bool resolved = SHGetPathFromIDListW(picked, selected) == TRUE;
		CoTaskMemFree(picked);
		if (!resolved)
			return false;

		outPath = PathText::FromPath(std::filesystem::path(selected));
		return true;
	}
#endif

} // namespace

void FileBrowserView::Render(const ImVec2& panelMin, const ImVec2& panelMax) {
	ConsumeFolderDrop(panelMin, panelMax);

	RenderToolbar();
	RenderSearchField();

	ImGui::BeginChild("FileTree");

	// right-clicking a row opens that row's menu; this one belongs to the empty space
	// around them, which is the only place to reach "add" without aiming at a button
	if (ImGui::BeginPopupContextWindow("##BrowserMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
		if (ImGui::MenuItem("Add Folder..."))
			AddFolder();
		// the picker cannot reach a UNC share that is not mapped, and a path is often
		// already on the clipboard, so typing one stays available next to it
		if (ImGui::MenuItem("Add Path..."))
			mOpenAddFolderPopup = true;
		if (ImGui::MenuItem("Refresh All"))
			mContext.fileBrowser.Refresh();
		ImGui::Separator();
		AppConfig& config = AppConfig::Instance();
		if (ImGui::MenuItem("Preview on Click", nullptr, config.libraryPreview)) {
			config.libraryPreview = !config.libraryPreview;
			config.Save();
			if (!config.libraryPreview)
				mContext.engine.GetPreviewPlayer().Stop();
		}
		ImGui::EndPopup();
	}

	// the arrow keys walk the rows this frame drew, so the list is rebuilt as they
	// are drawn rather than derived afterwards - only the tree knows what is open
	mVisibleRows.clear();
	if (mContext.fileBrowser.GetSearchQuery().empty())
		RenderTree();
	else
		RenderSearchResults();

	// consumed by the rows above; anything the keys set below belongs to next frame
	mScrollToSelection = false;
	HandleArrowKeys();

	ImGui::EndChild();

	// opened outside the child so the popup is positioned against the library window
	// rather than clipped to the scrolling region that asked for it
	if (mOpenAddFolderPopup) {
		ImGui::OpenPopup("Add Folder");
		mOpenAddFolderPopup = false;
	}
	if (ImGui::BeginPopup("Add Folder")) {
		ImGui::TextUnformatted("Folder path");
		ImGui::SetNextItemWidth(360.0f * mContext.state.mainScale);
		const bool submitted = ImGui::InputText("##TypedFolder", mTypedPathBuffer, IM_ARRAYSIZE(mTypedPathBuffer),
												ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button("Add") || submitted) {
			if (mContext.fileBrowser.AddRoot(mTypedPathBuffer))
				PersistRoots();
			mTypedPathBuffer[0] = '\0';
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

// ================================================================
// HEADER
// ================================================================

void FileBrowserView::RenderToolbar() {
	const Theme& th = Theme::Instance();

	ImGui::PushStyleColor(ImGuiCol_Text, th.textMuted);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("FILES");
	ImGui::PopStyleColor();

	// the add button is pinned to the right edge rather than following the label:
	// this column is narrow enough that a button floating mid-row reads as clutter
	const float buttonSize = ImGui::GetFrameHeight();
	const float buttonX = ImGui::GetContentRegionMax().x - buttonSize;
	ImGui::SameLine();
	if (buttonX > ImGui::GetCursorPosX())
		ImGui::SetCursorPosX(buttonX);
	if (ImGui::Button("+", ImVec2(buttonSize, buttonSize)))
		AddFolder();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Add a folder to the library");

	ImGui::Separator();
}

void FileBrowserView::RenderSearchField() {
	FileBrowser& browser = mContext.fileBrowser;

	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	// searching sweeps every root recursively, so it starts on the return key rather
	// than on each letter - one keystroke would otherwise cancel and restart a walk
	// of the whole sample drive
	if (ImGui::InputTextWithHint("##fileSearch", "Search files", mSearchBuffer, IM_ARRAYSIZE(mSearchBuffer),
								 ImGuiInputTextFlags_EnterReturnsTrue)) {
		if (mSearchBuffer[0] == '\0')
			browser.CancelSearch();
		else
			browser.Search(mSearchBuffer);
	}
	// clearing the box puts the tree back without waiting for a return
	if (mSearchBuffer[0] == '\0' && !browser.GetSearchQuery().empty())
		browser.CancelSearch();
}

// ================================================================
// TREE
// ================================================================

void FileBrowserView::RenderTree() {
	FileBrowser& browser = mContext.fileBrowser;
	const Theme& th = Theme::Instance();

	const auto& roots = browser.GetRoots();
	if (roots.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, th.textDim);
		ImGui::TextWrapped("Add a folder of samples or projects with +, or drop one here.");
		ImGui::PopStyleColor();
		return;
	}

	int rootToRemove = -1;
	for (int i = 0; i < (int)roots.size(); ++i) {
		const std::string root = roots[i];
		ImGui::PushID(i);

		FileEntry rootRow;
		rootRow.path = root;
		rootRow.name = RootLabel(root);
		rootRow.kind = FileKind::Folder;
		mVisibleRows.push_back(rootRow);

		ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
		if (mSelectedPath == root)
			rootFlags |= ImGuiTreeNodeFlags_Selected;
		ApplyKeyboardState(root);

		const bool open = ImGui::TreeNodeEx(rootRow.name.c_str(), rootFlags);
		if (ImGui::IsItemClicked())
			mSelectedPath = root;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", root.c_str());

		if (ImGui::BeginPopupContextItem("##RootMenu")) {
			if (ImGui::MenuItem("Refresh"))
				browser.Refresh();
			if (ImGui::MenuItem("Show in Explorer"))
				ShowInFileManager(root, true);
			ImGui::Separator();
			if (ImGui::MenuItem("Remove from Library"))
				rootToRemove = i;
			ImGui::EndPopup();
		}

		if (open) {
			RenderDirectory(root);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}

	// after the loop: removing mid-iteration would shift the roots the rest of the
	// frame is drawing from under it
	if (rootToRemove >= 0) {
		browser.RemoveRoot(rootToRemove);
		PersistRoots();
	}
}

void FileBrowserView::RenderDirectory(const std::string& directory) {
	FileBrowser& browser = mContext.fileBrowser;

	// the listing is cached, and a cached listing is a node in a map - recursing into
	// a child cannot move the vector this loop is walking
	const std::vector<FileEntry>& entries = browser.List(directory);
	if (entries.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Instance().textDim);
		ImGui::TextUnformatted("empty");
		ImGui::PopStyleColor();
		return;
	}

	for (const auto& entry : entries) {
		if (entry.kind != FileKind::Folder) {
			RenderFile(entry);
			continue;
		}

		mVisibleRows.push_back(entry);

		ImGui::PushID(entry.path.c_str());
		ImGuiTreeNodeFlags folderFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
		if (mSelectedPath == entry.path)
			folderFlags |= ImGuiTreeNodeFlags_Selected;
		ApplyKeyboardState(entry.path);

		const bool open = ImGui::TreeNodeEx(entry.name.c_str(), folderFlags);
		if (ImGui::IsItemClicked())
			mSelectedPath = entry.path;
		if (ImGui::BeginPopupContextItem("##FolderMenu")) {
			if (ImGui::MenuItem("Refresh"))
				browser.Refresh(entry.path);
			if (ImGui::MenuItem("Show in Explorer"))
				ShowInFileManager(entry.path, true);
			ImGui::EndPopup();
		}
		if (open) {
			RenderDirectory(entry.path);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
}

void FileBrowserView::RenderFile(const FileEntry& entry) {
	mVisibleRows.push_back(entry);

	const PreviewPlayer& preview = mContext.engine.GetPreviewPlayer();
	const bool auditioning = preview.IsPlaying() && preview.PlayingPath() == entry.path;

	ImGui::PushID(entry.path.c_str());
	// the file being auditioned is the one row here that is happening rather than
	// merely selected, which is what the accent is for
	ImGui::PushStyleColor(ImGuiCol_Text, auditioning ? Theme::Instance().accent : KindColor(entry.kind));
	ApplyKeyboardState(entry.path);

	// a leaf sits where a folder's label would, so the indent guide reads straight
	// down a mixed listing instead of stepping in at every file
	if (ImGui::Selectable(entry.name.c_str(), mSelectedPath == entry.path, ImGuiSelectableFlags_AllowDoubleClick)) {
		mSelectedPath = entry.path;
		// a double-click arrives as a click first, so the audition it started is
		// stopped again on the way past - the file is about to be on a track anyway
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			mContext.engine.GetPreviewPlayer().Stop();
			OpenEntry(entry);
		} else if (AppConfig::Instance().libraryPreview) {
			TogglePreview(entry);
		}
	}
	ImGui::PopStyleColor();

	// a project is opened, never dropped on a track, so it is not a drag source
	if (entry.kind != FileKind::Project && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("LIBRARY_FILE", entry.path.c_str(), entry.path.size() + 1);
		ImGui::TextUnformatted(entry.name.c_str());
		ImGui::TextDisabled("%s", FileBrowser::KindLabel(entry.kind));
		ImGui::EndDragDropSource();
	} else if (ImGui::IsItemHovered()) {
		const char* hint = "Drag onto a track, or double-click for a new one";
		if (entry.kind == FileKind::Project)
			hint = "Double-click to open";
		else if (entry.kind == FileKind::Audio && AppConfig::Instance().libraryPreview)
			hint = "Click to preview, drag onto a track, double-click for a new one";
		ImGui::SetTooltip("%s\n%s", entry.path.c_str(), hint);
	}

	if (ImGui::BeginPopupContextItem("##FileMenu")) {
		if (entry.kind == FileKind::Audio && ImGui::MenuItem(auditioning ? "Stop Preview" : "Preview"))
			TogglePreview(entry);
		if (ImGui::MenuItem(entry.kind == FileKind::Project ? "Open Project" : "Import to New Track"))
			OpenEntry(entry);
		if (ImGui::MenuItem("Show in Explorer"))
			ShowInFileManager(entry.path, false);
		ImGui::EndPopup();
	}
	ImGui::PopID();
}

void FileBrowserView::RenderSearchResults() {
	FileBrowser& browser = mContext.fileBrowser;
	const Theme& th = Theme::Instance();

	if (browser.IsSearching()) {
		ImGui::PushStyleColor(ImGuiCol_Text, th.textMuted);
		ImGui::TextUnformatted("Searching...");
		ImGui::PopStyleColor();
		return;
	}

	const std::vector<FileEntry> results = browser.GetSearchResults();
	if (results.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, th.textDim);
		ImGui::TextWrapped("Nothing matching \"%s\".", browser.GetSearchQuery().c_str());
		ImGui::PopStyleColor();
		return;
	}

	for (const auto& entry : results)
		RenderFile(entry);

	if (browser.SearchTruncated()) {
		ImGui::PushStyleColor(ImGuiCol_Text, th.textDim);
		ImGui::TextWrapped("First %d matches. Narrow the search to see the rest.", (int)FileBrowser::kMaxSearchResults);
		ImGui::PopStyleColor();
	}
}

// ================================================================
// KEYBOARD
// ================================================================

// everything a row has to do before it is drawn: honor a fold the left/right keys
// asked for, and bring the row the selection just moved to back into view
void FileBrowserView::ApplyKeyboardState(const std::string& path) {
	if (!mSetOpenPath.empty() && mSetOpenPath == path) {
		ImGui::SetNextItemOpen(mSetOpenValue);
		mSetOpenPath.clear();
	}
	if (mScrollToSelection && mSelectedPath == path)
		ImGui::SetScrollHereY(0.5f);
}

void FileBrowserView::HandleArrowKeys() {
	// the panel's own keys, only while the panel is the one being used - the piano
	// roll moves notes with the same two keys
	if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput)
		return;

	if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		mContext.engine.GetPreviewPlayer().Stop();
		return;
	}

	// a folder opens and closes under the selection without reaching for the mouse
	if (!mSelectedPath.empty() && (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow))) {
		for (const auto& row : mVisibleRows) {
			if (row.kind == FileKind::Folder && row.path == mSelectedPath) {
				mSetOpenPath = mSelectedPath;
				mSetOpenValue = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
				return;
			}
		}
	}

	// held down, the keys repeat: this is how a folder of a hundred samples is
	// listened through
	const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
	const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
	if ((!down && !up) || mVisibleRows.empty())
		return;

	int current = -1;
	for (int i = 0; i < (int)mVisibleRows.size(); ++i) {
		if (mVisibleRows[i].path == mSelectedPath) {
			current = i;
			break;
		}
	}

	// nothing selected yet: the first key press lands on the end the user came from
	int next = current;
	if (current < 0)
		next = down ? 0 : (int)mVisibleRows.size() - 1;
	else
		next = std::clamp(current + (down ? 1 : -1), 0, (int)mVisibleRows.size() - 1);

	if (next == current)
		return;

	const FileEntry& row = mVisibleRows[next];
	mSelectedPath = row.path;
	mScrollToSelection = true;

	// the audition follows the selection: landing on a sample plays it, landing on
	// anything else stops whatever the last one was
	if (!AppConfig::Instance().libraryPreview)
		return;
	if (row.kind == FileKind::Audio)
		mContext.engine.GetPreviewPlayer().Play(row.path);
	else
		mContext.engine.GetPreviewPlayer().Stop();
}

// ================================================================
// COMMANDS
// ================================================================

void FileBrowserView::TogglePreview(const FileEntry& entry) {
	PreviewPlayer& preview = mContext.engine.GetPreviewPlayer();

	// the audition follows the selection, so picking a row that is not a sample ends
	// it - the same thing the arrow keys do on the way past one
	if (entry.kind != FileKind::Audio) {
		preview.Stop();
		return;
	}

	// clicking the row that is already playing is the stop button this panel would
	// otherwise need a control for
	if (preview.IsPlaying() && preview.PlayingPath() == entry.path)
		preview.Stop();
	else
		preview.Play(entry.path);
}

void FileBrowserView::OpenEntry(const FileEntry& entry) {
	if (entry.kind == FileKind::Project) {
		// the editor owns loading: it has to reset the undo history and pull the view
		// state out of the file, and this runs mid-frame inside somebody else's window
		mContext.state.pendingProjectPath = entry.path;
		return;
	}

	Project* project = mContext.GetProject();
	if (!project)
		return;

	// the insert marker is where the user is looking, and is where a paste lands too
	const double startBeat = mContext.state.selectionStart;

	auto before = TrackTopologyAction::Snapshot(project);
	const int trackIndex = LibraryImport::ImportToNewTrack(project, entry.path, startBeat);
	if (trackIndex < 0)
		return; // the file did not load; nothing changed, so nothing to undo

	mContext.undoManager.Push(std::make_unique<TrackTopologyAction>(
		project, std::move(before), TrackTopologyAction::Snapshot(project), "Import file"));
	mContext.state.SelectTrack(trackIndex);
}

void FileBrowserView::PersistRoots() {
	AppConfig& config = AppConfig::Instance();
	config.libraryFolders = mContext.fileBrowser.GetRoots();
	config.Save();
}

void FileBrowserView::AddFolder() {
#ifdef _WIN32
	std::string picked;
	if (PickFolder(mContext.nativeWindowHandle, picked)) {
		if (mContext.fileBrowser.AddRoot(picked))
			PersistRoots();
	}
#else
	// no native picker here, so the path is typed. the popup cannot be opened from
	// inside the child window this may be called from, hence the flag
	mOpenAddFolderPopup = true;
#endif
}

void FileBrowserView::ConsumeFolderDrop(const ImVec2& panelMin, const ImVec2& panelMax) {
	EditorState& state = mContext.state;
	if (!state.processDrop)
		return;
	if (state.dropX < panelMin.x || state.dropX > panelMax.x || state.dropY < panelMin.y || state.dropY > panelMax.y)
		return;

	// dropping a folder onto the panel is the shortest way to point the library at
	// one; a file dropped here is left for whoever else is watching the drop
	if (FileBrowser::Classify(state.droppedPath) != FileKind::Folder)
		return;

	if (mContext.fileBrowser.AddRoot(state.droppedPath))
		PersistRoots();
	state.processDrop = false;
}
