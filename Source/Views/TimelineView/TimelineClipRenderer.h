#pragma once
#include "EditorContext.h"
#include "TimelineDefs.h"
#include "Track.h"
#include "imgui.h"

class TimelineClipRenderer {
public:
	// standard render call. rowHeight is this track's visible lane height (shrinks for
	// a minimized/collapsed track), so clips draw to fit the row
	static void Render(EditorContext& context, TimelineInteractionState& interaction,
					   PendingClipMove& pendingMove, PendingClipDelete& pendingDelete,
					   Track* track, int trackIndex,
					   const ImVec2& winPos, float contentWidth, float viewWidth, float scrollX, float yPos, float rowHeight);

	// static helper to draw visual content of a clip (used for the ghost clip preview in trackview)
	// single source of truth for clip rendering. an opaque base color draws a clip proper -
	// a strip in that color over a body tinting the lane, lifted when selected; a
	// translucent one draws a flat reference copy (a drag ghost, an automation backdrop)
	static void DrawClipContent(ImDrawList* drawList,
								std::shared_ptr<Clip> clip,
								const ImVec2& pMin, const ImVec2& pMax,
								const ImVec2& winPos, float viewWidth,
								EditorContext& context,
								ImU32 baseColor,
								bool selected,
								double overrideStartBeat = -1.0,
								double overrideDuration = -1.0,
								double overrideOffset = -1.0,
								ImU32 customWaveColor = 0, // 0 for default
								ImU32 customMIDIColor = 0  // 0 for default
	);
};
