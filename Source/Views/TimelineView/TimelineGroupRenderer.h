#pragma once
#include "EditorContext.h"
#include "TimelineDefs.h"
#include "Track.h"
#include "imgui.h"

// the timeline lane of a group track. a group is a mixing container and never
// holds clips of its own, so this lane takes no drops, offers no clip menu and
// draws nothing editable - unlike TimelineClipRenderer, which owns clip lanes
//
// folded, the lane stands in for the rows it hides: every descendant's clips are
// drawn stacked as slim bands in their own track colors, so a collapsed group is
// still readable at a glance instead of being an empty strip
class TimelineGroupRenderer {
public:
	static void Render(EditorContext& context, Track* track, int trackIndex,
					   const ImVec2& winPos, float viewWidth, float scrollX, float yPos, float rowHeight);
};
