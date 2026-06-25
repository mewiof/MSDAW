#pragma once
#include "EditorContext.h"
#include "imgui.h"

class TrackListView {
public:
	TrackListView(EditorContext& context)
		: mContext(context) {}

	// expects coordinates relative to the master arrangement view
	void Render(const ImVec2& fixedPos, float width, float height, float trackAreaStartY, float stickyY, float masterY);
private:
	EditorContext& mContext;

	// track rename state
	int mRenamingIndex = -1;
	char mRenameBuf[256] = {0};
};
