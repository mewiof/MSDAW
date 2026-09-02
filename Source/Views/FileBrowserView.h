#pragma once
#include "EditorContext.h"
#include "Library/FileBrowser.h"
#include "imgui.h"

#include <string>
#include <vector>

// the library panel's file explorer: the folders the user pointed at, drawn as
// collapsible trees of the audio, MIDI and project files under them
//
// unlike the views in this folder it opens no window of its own - the library owns
// the column, and this draws into whatever height it is handed - so it takes no
// position, only the space it is given by the caller's cursor
class FileBrowserView {
public:
	FileBrowserView(EditorContext& context)
		: mContext(context) {}

	// `panelMin`/`panelMax` are the library window's own rect in screen space, used
	// to decide whether a folder dragged in from the OS landed on this panel
	void Render(const ImVec2& panelMin, const ImVec2& panelMax);
private:
	void RenderToolbar();
	void RenderSearchField();
	void RenderTree();
	void RenderDirectory(const std::string& directory);
	void RenderFile(const FileEntry& entry);
	void RenderSearchResults();

	// up/down walk the rows the tree is currently showing, auditioning each audio
	// file as it is landed on - the way a sample folder is actually browsed. left
	// and right fold the folder under the selection, escape stops the audition
	void HandleArrowKeys();

	// what a row has to be told before it is drawn: a fold the keys asked for, and
	// the scroll that keeps a keyboard-moved selection on screen
	void ApplyKeyboardState(const std::string& path);

	// audition a file; the same file again stops it, and a row that is not a sample
	// ends whatever was playing
	void TogglePreview(const FileEntry& entry);

	// a file the user asked for by double-click or context menu, rather than by
	// dragging it somewhere: audio and MIDI land on a track of their own at the
	// start of the arrangement, a project replaces what is open
	void OpenEntry(const FileEntry& entry);

	// the folders the browser holds are the ones the config remembers, so every
	// add/remove writes the list straight back out
	void PersistRoots();

	void AddFolder(); // native picker where there is one, a typed path otherwise
	void ConsumeFolderDrop(const ImVec2& panelMin, const ImVec2& panelMax);

	EditorContext& mContext;

	char mSearchBuffer[128] = "";
	std::string mSelectedPath; // drawn highlighted, and what the arrow keys step from

	// the rows drawn this frame, top to bottom, as the arrow keys see them: open
	// folders included, closed ones' contents not. rebuilt every frame because
	// folding a folder changes what "the next row" means
	std::vector<FileEntry> mVisibleRows;

	// set when a keyboard move needs the list scrolled to follow it
	bool mScrollToSelection = false;

	// a folder the left/right keys asked to fold or unfold, applied when the row is
	// next drawn - a tree node's open state is imgui's to keep, not ours
	std::string mSetOpenPath;
	bool mSetOpenValue = false;

	bool mOpenAddFolderPopup = false;
	char mTypedPathBuffer[512] = "";
};
