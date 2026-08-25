#include "PrecompHeader.h"
#include "TimelineGroupRenderer.h"
#include "Project.h"
#include "Theme.h"
#include <algorithm>
#include <string>
#include <vector>

namespace {

	// the tracks this lane stands in for, in project order:
	// - the group itself, but only if it is carrying clips. that can no longer happen
	//   by hand; a project saved while the timeline still allowed it would otherwise
	//   have those clips sound with nothing on screen to explain them
	// - while folded, every clip-owning track the fold is hiding. nested groups are
	//   containers of their own, so they are walked through rather than drawn
	std::vector<std::shared_ptr<Track>> SummarizedTracks(Project* project, Track* group, bool folded) {
		std::vector<std::shared_ptr<Track>> result;
		for (const auto& t : project->GetTracks()) {
			if (t.get() == group) {
				if (!t->GetClips().empty())
					result.push_back(t);
				continue;
			}
			if (t->IsGroup() || !folded)
				continue;
			for (auto p = t->GetParent(); p; p = p->GetParent()) {
				if (p.get() == group) {
					result.push_back(t);
					break;
				}
			}
		}
		return result;
	}

} // namespace

void TimelineGroupRenderer::Render(EditorContext& context, Track* track, int trackIndex,
								   const ImVec2& winPos, float viewWidth, float scrollX, float yPos, float rowHeight) {

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const Theme& th = Theme::Instance();
	Project* project = context.GetProject();

	// clicking the lane selects the group, the same as clicking its row in the track
	// list. nothing else is interactive here: a group takes no clips
	ImGui::SetCursorScreenPos(ImVec2(winPos.x + scrollX, yPos));
	ImGui::SetNextItemAllowOverlap();
	if (ImGui::InvisibleButton(("##GroupLane" + std::to_string(trackIndex)).c_str(), ImVec2(viewWidth, rowHeight))) {
		context.state.selectedClip = nullptr;
		context.state.SelectTrack(trackIndex);
	}

	if (!project)
		return;

	// unfolded, the children are drawn in their own rows right below; summarizing them
	// here as well would only duplicate what is already on screen
	std::vector<std::shared_ptr<Track>> bands = SummarizedTracks(project, track, track->mIsCollapsed);
	if (bands.empty())
		return;

	float padding = 2.0f;
	float usable = rowHeight - padding * 2.0f;
	if (usable <= 0.0f)
		return;

	// one band per summarized track. below ~2px a band stops reading as a bar at all,
	// so a deep group overflows its share rather than drawing invisible hairlines
	float bandHeight = std::max(2.0f, usable / (float)bands.size());

	for (size_t b = 0; b < bands.size(); ++b) {
		const auto& banded = bands[b];
		float bandTop = yPos + padding + bandHeight * (float)b;
		if (bandTop >= yPos + rowHeight - padding)
			break;
		float bandBottom = std::min(bandTop + bandHeight - 1.0f, yPos + rowHeight - padding);
		if (bandBottom <= bandTop)
			continue;

		for (const auto& clip : banded->GetClips()) {
			float clipStartX = winPos.x + (float)(clip->GetStartBeat() * context.state.pixelsPerBeat);
			float clipEndX = clipStartX + (float)(clip->GetDuration() * context.state.pixelsPerBeat);
			if (clipEndX <= winPos.x + scrollX || clipStartX >= winPos.x + scrollX + viewWidth)
				continue;

			// a deactivated clip greys out here too, so folding a group never hides the
			// fact that part of it is silent
			ImU32 color = clip->IsEnabled() ? banded->GetColor() : th.clipDisabled;
			drawList->AddRectFilled(ImVec2(clipStartX, bandTop), ImVec2(clipEndX, bandBottom), color);
		}
	}
}
