#include "PrecompHeader.h"
#include "TimelineTrackView.h"
#include "Project.h"
#include "Track.h"
#include "ProcessorFactory.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"
#include <algorithm>
#include <cmath>

#include "TimelineClipRenderer.h"
#include "TimelineGroupRenderer.h"
#include "TimelineAutomationRenderer.h"
#include "TrackLayout.h"
#include "Theme.h"

void TimelineTrackView::RenderTracks(EditorContext& context, TimelineInteractionState& interaction,
									 PendingClipMove& pendingMove, PendingClipDelete& pendingDelete,
									 const ImVec2& winPos, float contentWidth, float viewWidth, float scrollX, float startY) {

	const Theme& th = Theme::Instance();
	Project* project = context.GetProject();
	auto& tracks = project->GetTracks();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	auto rows = TrackLayout::Build(context);

	for (size_t i = 0; i < tracks.size(); ++i) {
		if (!rows[i].visible)
			continue; // hidden inside a folded group
		float yPos = startY + rows[i].top;
		float rowH = rows[i].height;
		auto& t = tracks[i];
		ImVec2 trackMin(winPos.x, yPos);
		ImVec2 trackMax(winPos.x + contentWidth, yPos + rowH);

		// track background. the selected track is lifted here as well as in the track
		// list - the arrangement is where the eye is while editing, and "which lane am I
		// pasting into" was only answerable by looking away at the list
		bool laneSelected = ((int)i == context.state.selectedTrackIndex) || context.state.multiSelectedTracks.count((int)i) > 0;
		ImU32 laneColor = t->IsGroup() ? th.bgLaneGroup : th.bgLane;
		if (laneSelected)
			laneColor = th.bgLaneSelected;
		drawList->AddRectFilled(trackMin, trackMax, laneColor);
		drawList->AddRect(trackMin, trackMax, th.border);

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
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PROCESSOR_MOVE")) {
				DeviceMovePayload* moveData = (DeviceMovePayload*)payload->Data;
				if (moveData->trackIndex != (int)i) {
					std::shared_ptr<Track> srcTrack = nullptr;
					if (moveData->trackIndex == -1)
						srcTrack = project->GetMasterTrack();
					else if (moveData->trackIndex >= 0 && moveData->trackIndex < (int)project->GetTracks().size())
						srcTrack = project->GetTracks()[moveData->trackIndex];

					if (srcTrack && moveData->deviceIndex < (int)srcTrack->GetProcessors().size()) {
						auto proc = srcTrack->GetProcessors()[moveData->deviceIndex];
						srcTrack->RemoveProcessor(moveData->deviceIndex);
						t->AddProcessor(proc);
					}
				}
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST_PLUGIN")) {
				std::string path = (const char*)payload->Data;
				auto vST = std::make_shared<VSTProcessor>(path);
				if (vST->Load()) {
					t->AddProcessor(vST);
					if (project->GetTransport().GetSampleRate() > 0)
						vST->PrepareToPlay(project->GetTransport().GetSampleRate());
				}
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST3_PLUGIN")) {
				std::string data = (const char*)payload->Data;
				size_t pipe = data.find('|');
				if (pipe != std::string::npos) {
					std::string path = data.substr(0, pipe);
					std::string classID = data.substr(pipe + 1);
					auto vST = std::make_shared<VST3Processor>(path, classID);
					if (vST->Load()) {
						t->AddProcessor(vST);
						if (project->GetTransport().GetSampleRate() > 0)
							vST->PrepareToPlay(project->GetTransport().GetSampleRate());
					}
				}
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INTERNAL_PLUGIN")) {
				std::string type = (const char*)payload->Data;
				std::shared_ptr<AudioProcessor> proc = ProcessorFactory::Instance().Create(type);

				if (proc) {
					t->AddProcessor(proc);
					if (project->GetTransport().GetSampleRate() > 0)
						proc->PrepareToPlay(project->GetTransport().GetSampleRate());
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

	// handles moving, resizingleft, and resizingright
	if (interaction.dragState != DragState::None && context.state.selectedClip) {

		// determine the ghost properties based on drag state
		double ghostStart = interaction.dragCurrentBeat;
		double ghostDuration = interaction.dragCurrentDuration;
		double ghostOffset = interaction.dragCurrentOffset;
		int ghostTrackIdx = -1;

		if (interaction.dragState == DragState::Moving) {
			ghostTrackIdx = interaction.dragTargetTrackIdx;
			// during move, duration/offset usually don't change
		} else if (interaction.dragState == DragState::ResizingLeft || interaction.dragState == DragState::ResizingRight) {
			// resize stays on the source track
			ghostTrackIdx = interaction.dragSourceTrackIdx;
		}

		if (ghostTrackIdx >= 0 && ghostTrackIdx < (int)rows.size() && rows[ghostTrackIdx].visible) {
			float ghostY = startY + rows[ghostTrackIdx].top;

			float clipStartX = winPos.x + (float)(ghostStart * context.state.pixelsPerBeat);
			float clipWidth = (float)(ghostDuration * context.state.pixelsPerBeat);
			float clipEndX = clipStartX + clipWidth;

			// calculate target rect
			ImVec2 pMin(clipStartX, ghostY + 1);
			ImVec2 pMax(clipEndX, ghostY + rows[ghostTrackIdx].height - 1);

			// use highlight color for preview
			ImU32 ghostColor = th.ghost;

			// draw using helper, passing overrides for start, duration, and offset
			TimelineClipRenderer::DrawClipContent(drawList, context.state.selectedClip,
												  pMin, pMax, winPos, viewWidth, context, ghostColor,
												  ghostStart, ghostDuration, ghostOffset);
		}
	}
}
