#pragma once
#include "EditorContext.h"
#include "TimelineDefs.h"
#include "Track.h"
#include "imgui.h"

// the timeline lane of a group track. a group is a mixing container and never
// holds clips of its own, so this lane takes no drops and offers no clip menu -
// unlike TimelineClipRenderer, which owns the lanes that do
class TimelineGroupRenderer {
public:
	static void Render(EditorContext& context, Track* track, int trackIndex,
					   const ImVec2& winPos, float viewWidth, float scrollX, float yPos, float rowHeight);
};
