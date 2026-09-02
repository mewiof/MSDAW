#include "PrecompHeader.h"
#include "TimelineTrackView.h"
#include "Project.h"
#include "Track.h"
#include "ProcessorFactory.h"
#include "Processors/RackProcessor.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"
#include <algorithm>
#include <cmath>

#include "TimelineClipRenderer.h"
#include "TimelineClipOps.h"
#include "TimelineGroupRenderer.h"
#include "TimelineAutomationRenderer.h"
#include "TrackLayout.h"
#include "Theme.h"
#include "Views/DeviceRackOps.h"

void TimelineTrackView::RenderTracks(EditorContext& context, TimelineInteractionState& interaction,
									 PendingClipMove& pendingMove, PendingClipDelete& pendingDelete,
									 const ImVec2& winPos, float contentWidth, float viewWidth, float scrollX, float startY) {

	const Theme& th = Theme::Instance();
	Project* project = context.GetProject();
	auto& tracks = project->GetTracks();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	auto rows = TrackLayout::Build(context);

	// the lane fills say which track is selected, and clicking a clip is one of the
	// things that changes that - but the fill for a lane is emitted before that lane's
	// clips have taken any input, so reading the selection here would always be a frame
	// behind. split the draw list, let the loop below build the content into the upper
	// channel, and paint the fills into the lower one afterwards, once every click this
	// frame has landed. merged at the end they still sit underneath, and the highlight
	// now moves on the same frame as the click
	drawList->ChannelsSplit(2);
	drawList->ChannelsSetCurrent(1);

	for (size_t i = 0; i < tracks.size(); ++i) {
		if (!rows[i].visible)
			continue; // hidden inside a folded group
		float yPos = startY + rows[i].top;
		float rowH = rows[i].height;
		auto& t = tracks[i];
		ImVec2 trackMin(winPos.x, yPos);
		ImVec2 trackMax(winPos.x + contentWidth, yPos + rowH);

		// NOTE: the track background is not drawn here - it is deferred to the lower
		// draw-list channel after this loop, so its selection highlight is same-frame

		// grid logic
		if (context.state.timelineGrid > 0.0) {
			double grid = context.state.timelineGrid;

			double startVis = scrollX / context.state.pixelsPerBeat;
			double endVis = (scrollX + viewWidth) / context.state.pixelsPerBeat;

			// avoid floating point errors
			int startIdx = (int)floor(startVis / grid);
			int endIdx = (int)ceil(endVis / grid);

			for (int i = startIdx; i <= endIdx; ++i) {
				double b = i * grid;
				// skip if out of view (floor/ceil padding)
				if (b < startVis - grid)
					continue;

				float x = winPos.x + (float)(b * context.state.pixelsPerBeat);

				bool isBar = std::abs(fmod(b + 0.001, 4.0)) < 0.002;

				ImU32 gridCol = isBar ? th.gridBar : th.gridBeat;
				drawList->AddLine(ImVec2(x, trackMin.y), ImVec2(x, trackMax.y), gridCol);
			}
		}

		// device drag & drop target (cross-track)
		ImGui::SetCursorScreenPos(ImVec2(winPos.x + scrollX, trackMin.y));
		ImGui::PushID((int)i * 20000);
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##TrackDropTarget", ImVec2(viewWidth, rowH));

		if (ImGui::BeginDragDropTarget()) {
			// a device dropped on a lane lands at the end of that track's chain, through
			// the same operations the rack view uses - so it takes the project lock and
			// lands in the history exactly like a drop inside the rack does
			const int chainEnd = (int)t->GetProcessors().size();
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PROCESSOR_MOVE")) {
				const DeviceRackOps::DevicePath source = *(const DeviceRackOps::DevicePath*)payload->Data;
				auto location = DeviceRackOps::ResolveDevice(project, source);
				if (location.IsValid() && location.host != t)
					DeviceRackOps::MoveDevice(project, context.undoManager, location.host, location.index, t, chainEnd);
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST_PLUGIN")) {
				std::string path = (const char*)payload->Data;
				auto vST = std::make_shared<VSTProcessor>(path);
				if (vST->Load())
					DeviceRackOps::InsertDevice(project, context.undoManager, t, chainEnd, vST, "Add device");
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST3_PLUGIN")) {
				std::string data = (const char*)payload->Data;
				size_t pipe = data.find('|');
				if (pipe != std::string::npos) {
					auto vST = std::make_shared<VST3Processor>(data.substr(0, pipe), data.substr(pipe + 1));
					if (vST->Load())
						DeviceRackOps::InsertDevice(project, context.undoManager, t, chainEnd, vST, "Add device");
				}
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INTERNAL_PLUGIN")) {
				if (auto proc = ProcessorFactory::Instance().Create((const char*)payload->Data)) {
					if (auto rack = std::dynamic_pointer_cast<RackProcessor>(proc))
						rack->AddChain("Chain");
					DeviceRackOps::InsertDevice(project, context.undoManager, t, chainEnd, proc, "Add device");
				}
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::PopID();

		ImGui::SetCursorScreenPos(trackMin);
		ImGui::PushID((int)i * 10000);

		// a minimized (thin) track shows its clip lane rather than a cramped automation lane
		bool minimized = t->mIsCollapsed && !t->IsGroup();
		if (t->mShowAutomation && !minimized) {
			TimelineAutomationRenderer::Render(context, interaction, t.get(), (int)i, winPos, contentWidth, viewWidth, scrollX, yPos);
		} else if (t->IsGroup()) {
			// a group has no clip lane of its own; folded, it stands in for the rows it hides
			TimelineGroupRenderer::Render(context, t.get(), (int)i, winPos, viewWidth, scrollX, yPos, rowH);
		} else {
			TimelineClipRenderer::Render(context, interaction, pendingMove, pendingDelete, t.get(), (int)i, winPos, contentWidth, viewWidth, scrollX, yPos, rowH);
		}

		ImGui::PopID(); // track id
	}

	// deferred lane backgrounds. the selected track is lifted here as well as in the
	// track list - the arrangement is where the eye is while editing, and "which lane
	// am I pasting into" was only answerable by looking away at the list
	drawList->ChannelsSetCurrent(0);
	for (size_t i = 0; i < tracks.size(); ++i) {
		if (!rows[i].visible)
			continue;
		ImVec2 trackMin(winPos.x, startY + rows[i].top);
		ImVec2 trackMax(winPos.x + contentWidth, startY + rows[i].top + rows[i].height);

		bool laneSelected = ((int)i == context.state.selectedTrackIndex) || context.state.multiSelectedTracks.count((int)i) > 0;
		ImU32 laneColor = tracks[i]->IsGroup() ? th.bgLaneGroup : th.bgLane;
		if (laneSelected)
			laneColor = th.bgLaneSelected;
		drawList->AddRectFilled(trackMin, trackMax, laneColor);
		drawList->AddRect(trackMin, trackMax, th.border);
	}
	drawList->ChannelsMerge();

	// ================================================================
	// CLIP MARQUEE
	// ================================================================
	// a rubber band dragged over empty lane space. it is resolved here rather than in
	// the per-track renderer that starts it, because the box spans tracks and only this
	// level knows every row's band
	if (interaction.clipMarqueeActive) {
		ImGuiIO& io = ImGui::GetIO();

		double endBeat = TimelineClipOps::SnapMarqueeBeat(context, (io.MousePos.x - winPos.x) / context.state.pixelsPerBeat);
		int endTrack = TrackLayout::RowAtY(rows, io.MousePos.y - startY);
		if (endTrack >= 0) {
			interaction.clipMarqueeEndBeat = endBeat;
			interaction.clipMarqueeEndTrack = endTrack;
		}

		// a press with no travel is a plain click on the background, not a box. the
		// threshold keeps a twitchy click from selecting whatever it grazed
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))
			interaction.clipMarqueeMoved = true;

		if (interaction.clipMarqueeMoved) {
			auto hits = TimelineClipOps::ClipsInBox(project,
													interaction.clipMarqueeStartTrack, interaction.clipMarqueeEndTrack,
													interaction.clipMarqueeStartBeat, interaction.clipMarqueeEndBeat);
			// the selection is rebuilt from scratch every frame so shrinking the box
			// gives back what it no longer covers; anything held before a modifier-drag
			// began is carried along in the base
			std::vector<std::shared_ptr<Clip>> selection = interaction.clipMarqueeBase;
			for (const auto& hit : hits) {
				if (std::find(selection.begin(), selection.end(), hit) == selection.end())
					selection.push_back(hit);
			}
			context.state.SetClipSelection(std::move(selection), context.state.selectedClip);

			int bandTop = std::clamp(std::min(interaction.clipMarqueeStartTrack, interaction.clipMarqueeEndTrack), 0, (int)rows.size() - 1);
			int bandBottom = std::clamp(std::max(interaction.clipMarqueeStartTrack, interaction.clipMarqueeEndTrack), 0, (int)rows.size() - 1);
			float boxX1 = winPos.x + (float)(std::min(interaction.clipMarqueeStartBeat, interaction.clipMarqueeEndBeat) * context.state.pixelsPerBeat);
			float boxX2 = winPos.x + (float)(std::max(interaction.clipMarqueeStartBeat, interaction.clipMarqueeEndBeat) * context.state.pixelsPerBeat);
			float boxY1 = startY + rows[bandTop].top;
			float boxY2 = startY + rows[bandBottom].top + rows[bandBottom].height;

			drawList->AddRectFilled(ImVec2(boxX1, boxY1), ImVec2(boxX2, boxY2), th.selectionFill);
			drawList->AddRect(ImVec2(boxX1, boxY1), ImVec2(boxX2, boxY2), th.selectionStroke);
		}

		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			// a click on empty space that never became a box drops the selection, the
			// way clicking away from a selection does everywhere else
			if (!interaction.clipMarqueeMoved)
				context.state.ClearClipSelection();
			interaction.clipMarqueeActive = false;
			interaction.clipMarqueeMoved = false;
			interaction.clipMarqueeBase.clear();
		}
	}

	// ================================================================
	// DRAG GHOSTS
	// ================================================================
	// handles moving, resizingleft, and resizingright. every clip in the selection
	// travels with the gesture, so each one gets its own preview at the position the
	// commit will actually put it - both go through ComputeDragGeometry
	if (interaction.dragState != DragState::None && interaction.dragMoved) {
		for (const auto& entry : interaction.dragEntries) {
			DraggedClipGeometry geom = TimelineClipOps::ComputeDragGeometry(context, interaction, entry);
			if (geom.trackIdx < 0 || geom.trackIdx >= (int)rows.size() || !rows[geom.trackIdx].visible)
				continue;

			float ghostY = startY + rows[geom.trackIdx].top;
			float clipStartX = winPos.x + (float)(geom.start * context.state.pixelsPerBeat);
			float clipWidth = std::max((float)(geom.duration * context.state.pixelsPerBeat), 1.0f);

			ImVec2 pMin(clipStartX, ghostY + 1);
			ImVec2 pMax(clipStartX + clipWidth, ghostY + rows[geom.trackIdx].height - 1);

			// draw using helper, passing overrides for start, duration, and offset
			TimelineClipRenderer::DrawClipContent(drawList, entry.clip,
												  pMin, pMax, winPos, viewWidth, context, th.ghost,
												  geom.start, geom.duration, geom.offset);
		}
	}
}
