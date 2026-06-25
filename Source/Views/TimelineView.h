#pragma once
#include "EditorContext.h"
#include "imgui.h"
#include <memory>
#include "Clip.h"
#include "TimelineView/TimelineDefs.h"

class TrackListView;

class TimelineView {
public:
	TimelineView(EditorContext& context)
		: mContext(context) {}
	void Render(const ImVec2& pos, float width, float height, TrackListView* trackListView, float trackListW);
private:
	EditorContext& mContext;
	TimelineInteractionState mInteraction;
};
