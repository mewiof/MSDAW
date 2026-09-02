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

	// was the arrangement window hovered last frame. the Ctrl+Wheel zoom is resolved
	// before Begin (so the scrollbar can be sized from this frame's zoom), which is
	// too early to ask ImGui::IsWindowHovered() - so we use last frame's answer
	bool mHoveredLastFrame = false;

	// middle button held: the view follows the mouse on both axes at once. the button
	// is grabbed while the arrangement is hovered and kept until it comes back up, so
	// a pan that wanders over the track list or off the panel entirely still tracks
	bool mPanning = false;

	// last frame's scroll-independent content origin (screen x of beat 0 at scroll 0).
	// the pre-Begin zoom anchor needs it to place the cursor in content space, but it
	// cannot call GetCursorScreenPos() before Begin exists - so we cache last frame's
	float mContentLeftX = 0.0f;
	bool mContentLeftValid = false;
};
