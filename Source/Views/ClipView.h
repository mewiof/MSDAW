#pragma once
#include "EditorContext.h"
#include "Clips/AudioClip.h"
#include "imgui.h"
#include <memory>

class ClipView {
public:
	ClipView(EditorContext& context)
		: mContext(context) {}
	void Render(const ImVec2& pos, float width, float height);
private:
	EditorContext& mContext;

	// one warp/pitch drag = one undo entry: grab the clip's state when a drag
	// begins, push the before->after action when it ends
	std::shared_ptr<AudioClip> mWarpGestureClip;
	AudioClipWarpState mWarpGestureBefore;
	bool mWarpGestureActive = false;
};
