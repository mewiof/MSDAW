#pragma once
#include "EditorContext.h"
#include "Views/FileBrowserView.h"
#include "imgui.h"

// the left-hand column: the devices and plugins that can be dropped on a track,
// and under them the file explorer over the folders the user pointed the library at
class LibraryView {
public:
	LibraryView(EditorContext& context)
		: mContext(context), mFileBrowserView(context) {}
	void Render(const ImVec2& pos, float width, float height);
private:
	void RenderInternalDevices();
	void RenderPlugins();

	// a draggable boundary between two of the column's sections. `sharedHeight` is the
	// space those two divide, which is what a drag moves the stored fraction against
	void RenderSplitter(const char* id, float& fraction, float sharedHeight);

	EditorContext& mContext;
	FileBrowserView mFileBrowserView;
};
