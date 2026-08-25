#include "PrecompHeader.h"
#include "TimelineGroupRenderer.h"
#include "Project.h"
#include "Theme.h"
#include <string>

void TimelineGroupRenderer::Render(EditorContext& context, Track* track, int trackIndex,
								   const ImVec2& winPos, float viewWidth, float scrollX, float yPos, float rowHeight) {

	(void)track;

	// clicking the lane selects the group, the same as clicking its row in the track
	// list. nothing else is interactive here: a group takes no clips
	ImGui::SetCursorScreenPos(ImVec2(winPos.x + scrollX, yPos));
	ImGui::SetNextItemAllowOverlap();
	if (ImGui::InvisibleButton(("##GroupLane" + std::to_string(trackIndex)).c_str(), ImVec2(viewWidth, rowHeight))) {
		context.state.selectedClip = nullptr;
		context.state.SelectTrack(trackIndex);
	}
}
