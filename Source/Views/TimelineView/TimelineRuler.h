#pragma once
#include "EditorContext.h"
#include "TimelineDefs.h"
#include "imgui.h"

class TimelineRuler {
public:
	static void Render(EditorContext& context, TimelineInteractionState& interaction,
					   const ImVec2& winPos, float contentWidth, float visibleWidth, float height, float scrollX);
};
