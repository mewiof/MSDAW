#include "PrecompHeader.h"
#include "TrackListView.h"
#include "Project.h"
#include "ProcessorFactory.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"
#include "Undo/Actions.h"
#include "Theme.h"
#include <algorithm>
#include <vector>
#include <cmath>
#include <cstring>
#include <mutex>

struct TrackMovePayload {
	int srcIndex;
};

void TrackListView::Render(const ImVec2& fixedPos, float width, float height, float trackAreaStartY, float stickyY, float masterY) {
	Project* project = mContext.GetProject();
	if (!project)
		return;

	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();

	float headerHeight = 30 * mContext.state.mainScale;
	float rowFullHeight = mContext.layout.trackRowHeight + mContext.layout.trackGap;
	float hScrollbarSize = ImGui::GetStyle().ScrollbarSize;

	auto& tracks = project->GetTracks();

	int trackToProcess = -1;
	enum Action { None,
				  Delete,
				  Ungroup };
	Action action = None;

	ImVec2 clipMin(fixedPos.x, stickyY + headerHeight);
	ImVec2 clipMax(fixedPos.x + width, masterY);
	ImGui::PushClipRect(clipMin, clipMax, true);

	for (size_t i = 0; i < tracks.size(); ++i) {
		ImGui::PushID((int)i);
		auto track = tracks[i];

		float curY = trackAreaStartY + i * rowFullHeight;

		if (curY + rowFullHeight < stickyY + headerHeight || curY > masterY) {
			ImGui::PopID();
			continue;
		}

		// drop target (reorder gap)
		ImGui::SetCursorScreenPos(ImVec2(fixedPos.x, curY));
		ImGui::InvisibleButton("##ReorderGap", ImVec2(width, 6.0f));
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TRACK_MOVE")) {
				TrackMovePayload* data = (TrackMovePayload*)payload->Data;
				if (data->srcIndex != (int)i) {
					TrackTopologyAction::Record(mContext.undoManager, project, "Move track", [&] {
						project->MoveTrack(data->srcIndex, (int)i, false);
					});
				}
			}
			ImGui::EndDragDropTarget();
		}

		bool isTrackSelected = (mContext.state.selectedTrackIndex == (int)i);
		if (mContext.state.multiSelectedTracks.count((int)i))
			isTrackSelected = true;

		ImVec2 curPos = ImVec2(fixedPos.x, curY + 4.0f);

		float indent = 0.0f;
		if (track->GetParent()) {
			indent = 15.0f * mContext.state.mainScale;
			auto p = track->GetParent();
			while (p->GetParent()) {
				indent += 15.0f * mContext.state.mainScale;
				p = p->GetParent();
			}
		}

		ImU32 bgCol = isTrackSelected ? th.bgActive : th.bgPanel;
		if (track->IsGroup())
			bgCol = isTrackSelected ? th.bgActive : th.bgPanelAlt; // groups read as slightly raised containers

		drawList->AddRectFilled(
			ImVec2(curPos.x + indent, curPos.y),
			ImVec2(curPos.x + width, curPos.y + mContext.layout.trackRowHeight),
			bgCol);

		ImU32 stripCol = track->GetColor();
		drawList->AddRectFilled(
			ImVec2(curPos.x + indent, curPos.y),
			ImVec2(curPos.x + indent + 6 * mContext.state.mainScale, curPos.y + mContext.layout.trackRowHeight),
			stripCol);

		ImGui::SetCursorScreenPos(ImVec2(curPos.x + indent, curPos.y));
		ImGui::SetNextItemAllowOverlap();

		if (ImGui::InvisibleButton("TrackSelect", ImVec2(width - indent - 45 * mContext.state.mainScale, mContext.layout.trackRowHeight))) {
			if (ImGui::GetIO().KeyCtrl) {
				if (mContext.state.multiSelectedTracks.count((int)i)) {
					mContext.state.multiSelectedTracks.erase((int)i);
				} else {
					mContext.state.multiSelectedTracks.insert((int)i);
					mContext.state.selectedTrackIndex = (int)i;
				}
			} else if (ImGui::GetIO().KeyShift) {
				int start = mContext.state.selectedTrackIndex;
				int end = (int)i;
				if (start > end)
					std::swap(start, end);
				for (int k = start; k <= end; ++k)
					mContext.state.multiSelectedTracks.insert(k);
				mContext.state.selectedTrackIndex = (int)i;
			} else {
				mContext.state.multiSelectedTracks.clear();
				mContext.state.multiSelectedTracks.insert((int)i);
				mContext.state.selectedTrackIndex = (int)i;
			}
		}

		if (ImGui::BeginDragDropSource()) {
			TrackMovePayload payload;
			payload.srcIndex = (int)i;
			ImGui::SetDragDropPayload("TRACK_MOVE", &payload, sizeof(TrackMovePayload));
			ImGui::Text("Move %s", track->GetName().c_str());
			ImGui::EndDragDropSource();
		}

		if (ImGui::BeginDragDropTarget()) {
			if (track->IsGroup()) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TRACK_MOVE")) {
					TrackMovePayload* data = (TrackMovePayload*)payload->Data;
					if (data->srcIndex != (int)i)
						TrackTopologyAction::Record(mContext.undoManager, project, "Move track", [&] {
							project->MoveTrack(data->srcIndex, (int)i, true);
						});
				}
			}

			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST_PLUGIN")) {
				std::string path = (const char*)payload->Data;
				auto vST = std::make_shared<VSTProcessor>(path);
				if (vST->Load()) {
					std::lock_guard<std::mutex> lock(project->GetMutex());
					track->AddProcessor(vST);
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
						std::lock_guard<std::mutex> lock(project->GetMutex());
						track->AddProcessor(vST);
						if (project->GetTransport().GetSampleRate() > 0)
							vST->PrepareToPlay(project->GetTransport().GetSampleRate());
					}
				}
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INTERNAL_PLUGIN")) {
				std::string type = (const char*)payload->Data;
				std::shared_ptr<AudioProcessor> proc = ProcessorFactory::Instance().Create(type);

				if (proc) {
					std::lock_guard<std::mutex> lock(project->GetMutex());
					track->AddProcessor(proc);
					if (project->GetTransport().GetSampleRate() > 0)
						proc->PrepareToPlay(project->GetTransport().GetSampleRate());
				}
			}
			ImGui::EndDragDropTarget();
		}

		if (ImGui::BeginPopupContextItem()) {
			if (ImGui::MenuItem("Rename")) {
				mRenamingIndex = (int)i;
				strcpy(mRenameBuf, track->GetName().c_str());
			}
			if (ImGui::BeginMenu("Color")) {
				ImVec4 col = ImGui::ColorConvertU32ToFloat4(track->GetColor());
				if (ImGui::ColorPicker4("##TrackColor", (float*)&col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_NoInputs)) {
					track->SetColor(ImGui::ColorConvertFloat4ToU32(col));
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Delete")) {
				trackToProcess = (int)i;
				action = Delete;
			}
			if (track->IsGroup()) {
				if (ImGui::MenuItem("Ungroup")) {
					trackToProcess = (int)i;
					action = Ungroup;
				}
			} else {
				if (!mContext.state.multiSelectedTracks.empty() && ImGui::MenuItem("Group Tracks (Ctrl+G)")) {
					TrackTopologyAction::Record(mContext.undoManager, project, "Group tracks", [&] {
						project->GroupSelectedTracks(mContext.state.multiSelectedTracks);
					});
					mContext.state.multiSelectedTracks.clear();
				}
			}
			ImGui::EndPopup();
		}

		ImGui::SetCursorScreenPos(ImVec2(curPos.x + indent + 15 * mContext.state.mainScale, curPos.y + 5 * mContext.state.mainScale));

		if (mRenamingIndex == (int)i) {
			ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(120 * mContext.state.mainScale);
			if (ImGui::InputText("##Rename", mRenameBuf, 256, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
				track->SetName(mRenameBuf);
				mRenamingIndex = -1;
			}
			if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				mRenamingIndex = -1;
			}
			if (ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered()) {
				if (mRenamingIndex != -1) {
					track->SetName(mRenameBuf);
					mRenamingIndex = -1;
				}
			}
		} else {
			std::string dispName = track->GetName();
			if (track->IsGroup())
				dispName = "[G] " + dispName;
			ImGui::Text("%s", dispName.c_str());

			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
				mRenamingIndex = (int)i;
				strcpy(mRenameBuf, track->GetName().c_str());
			}
		}

		ImGui::SetCursorScreenPos(ImVec2(curPos.x + indent + 15 * mContext.state.mainScale, curPos.y + 25 * mContext.state.mainScale));

		bool mute = track->GetMute();
		if (mute) {
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}
		if (ImGui::SmallButton("M"))
			track->SetMute(!mute);
		if (mute)
			ImGui::PopStyleColor(2);

		ImGui::SameLine();

		bool solo = track->GetSolo();
		if (solo)
			ImGui::PushStyleColor(ImGuiCol_Button, th.success);
		if (ImGui::SmallButton("S")) {
			bool keyMod = ImGui::GetIO().KeyCtrl;

			if (keyMod)
				track->SetSolo(!solo);
			else {
				if (solo)
					for (auto& t : project->GetTracks())
						t->SetSolo(false);
				else {
					for (auto& t : project->GetTracks())
						t->SetSolo(false);
					track->SetSolo(true);
				}
			}
		}
		if (solo)
			ImGui::PopStyleColor();

		ImGui::SameLine();

		bool showAuto = track->mShowAutomation;
		if (showAuto)
			ImGui::PushStyleColor(ImGuiCol_Button, th.danger);
		if (ImGui::SmallButton("A"))
			track->mShowAutomation = !showAuto;
		if (showAuto)
			ImGui::PopStyleColor();

		ImGui::SetCursorScreenPos(ImVec2(curPos.x + indent + 15 * mContext.state.mainScale, curPos.y + 42 * mContext.state.mainScale));
		ImGui::BeginChild("##VolParam", ImVec2(width - indent - 55 * mContext.state.mainScale, 35 * mContext.state.mainScale), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
		track->GetVolumeParameter()->Draw();
		ImGui::EndChild();

		float peakL = track->GetPeakL();
		float peakR = track->GetPeakR();
		float meterW = 6.0f * mContext.state.mainScale;
		float meterH = mContext.layout.trackRowHeight - 10.0f;
		float meterX = curPos.x + width - 35.0f * mContext.state.mainScale;
		float meterY = curPos.y + 5.0f;

		auto LinToNorm = [](float val) -> float {
			if (val <= 0.0001f)
				return 0.0f;
			float db = 20.0f * std::log10(val);
			float norm = (db + 60.0f) / 60.0f;
			return std::clamp(norm, 0.0f, 1.0f);
		};

		float hL = LinToNorm(peakL) * meterH;
		float hR = LinToNorm(peakR) * meterH;
		float normL = LinToNorm(peakL);
		float normR = LinToNorm(peakR);

		drawList->AddRectFilled(ImVec2(meterX, meterY), ImVec2(meterX + meterW, meterY + meterH), th.meterBg);
		drawList->AddRectFilled(ImVec2(meterX, meterY + meterH - hL), ImVec2(meterX + meterW * 0.5f, meterY + meterH), th.MeterColor(normL));
		drawList->AddRectFilled(ImVec2(meterX + meterW * 0.5f, meterY + meterH - hR), ImVec2(meterX + meterW, meterY + meterH), th.MeterColor(normR));

		ImGui::SetCursorScreenPos(ImVec2(curPos.x + width - 25 * mContext.state.mainScale, curPos.y + 5 * mContext.state.mainScale));
		if (ImGui::Button("X", ImVec2(16 * mContext.state.mainScale, 16 * mContext.state.mainScale))) {
			trackToProcess = (int)i;
			action = Delete;
		}

		ImGui::PopID();
	}

	ImGui::SetCursorScreenPos(ImVec2(fixedPos.x, trackAreaStartY + tracks.size() * rowFullHeight));
	ImGui::InvisibleButton("##ReorderGapEnd", ImVec2(width, 10.0f));
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TRACK_MOVE")) {
			TrackMovePayload* data = (TrackMovePayload*)payload->Data;
			TrackTopologyAction::Record(mContext.undoManager, project, "Move track", [&] {
				project->MoveTrack(data->srcIndex, (int)tracks.size(), false);
			});
		}
		ImGui::EndDragDropTarget();
	}

	ImGui::PopClipRect();

	drawList->AddRectFilled(ImVec2(fixedPos.x, stickyY), ImVec2(fixedPos.x + width, stickyY + headerHeight), th.bgHeader);
	drawList->AddLine(ImVec2(fixedPos.x, stickyY + headerHeight), ImVec2(fixedPos.x + width, stickyY + headerHeight), th.borderStrong);

	ImGui::SetCursorScreenPos(ImVec2(fixedPos.x + 8 * mContext.state.mainScale, stickyY + 4 * mContext.state.mainScale));
	if (ImGui::Button("+ Add Track", ImVec2(width - 16 * mContext.state.mainScale, 22 * mContext.state.mainScale))) {
		TrackTopologyAction::Record(mContext.undoManager, project, "Add track", [&] {
			project->CreateTrack();
		});
	}

	if (trackToProcess != -1) {
		if (action == Delete) {
			TrackTopologyAction::Record(mContext.undoManager, project, "Delete track", [&] {
				project->RemoveTrack(trackToProcess);
			});
			mContext.state.multiSelectedTracks.clear();
			if (mContext.state.selectedTrackIndex >= (int)tracks.size())
				mContext.state.selectedTrackIndex = std::max(0, (int)tracks.size() - 1);
		} else if (action == Ungroup) {
			TrackTopologyAction::Record(mContext.undoManager, project, "Ungroup track", [&] {
				project->UngroupTrack(trackToProcess);
			});
		}
	}

	auto master = project->GetMasterTrack();
	if (master) {
		bool isMasterSelected = (mContext.state.selectedTrackIndex == -1);
		ImU32 bgCol = isMasterSelected ? th.bgActive : th.bgPanelAlt;
		ImVec2 curPos = ImVec2(fixedPos.x, masterY);

		float masterHeight = mContext.layout.trackRowHeight + mContext.layout.trackGap;

		drawList->AddRectFilled(curPos, ImVec2(curPos.x + width, curPos.y + masterHeight), bgCol);

		ImGui::SetCursorScreenPos(curPos);
		ImGui::SetNextItemAllowOverlap();

		if (ImGui::InvisibleButton("MasterSelect", ImVec2(width - 40 * mContext.state.mainScale, masterHeight))) {
			mContext.state.selectedTrackIndex = -1;
		}

		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST_PLUGIN")) {
				std::string path = (const char*)payload->Data;
				auto vST = std::make_shared<VSTProcessor>(path);
				if (vST->Load()) {
					std::lock_guard<std::mutex> lock(project->GetMutex());
					master->AddProcessor(vST);
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
						std::lock_guard<std::mutex> lock(project->GetMutex());
						master->AddProcessor(vST);
						if (project->GetTransport().GetSampleRate() > 0)
							vST->PrepareToPlay(project->GetTransport().GetSampleRate());
					}
				}
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INTERNAL_PLUGIN")) {
				std::string type = (const char*)payload->Data;
				std::shared_ptr<AudioProcessor> proc = ProcessorFactory::Instance().Create(type);

				if (proc) {
					std::lock_guard<std::mutex> lock(project->GetMutex());
					master->AddProcessor(proc);
					if (project->GetTransport().GetSampleRate() > 0)
						proc->PrepareToPlay(project->GetTransport().GetSampleRate());
				}
			}
			ImGui::EndDragDropTarget();
		}

		ImGui::SetCursorScreenPos(ImVec2(curPos.x + 10, curPos.y + 5));
		ImGui::Text("MASTER");

		ImGui::SetCursorScreenPos(ImVec2(curPos.x + 10, curPos.y + 25));
		bool showAuto = master->mShowAutomation;
		if (showAuto)
			ImGui::PushStyleColor(ImGuiCol_Button, th.danger);
		if (ImGui::SmallButton("A##Master"))
			master->mShowAutomation = !showAuto;
		if (showAuto)
			ImGui::PopStyleColor();

		ImGui::SetCursorScreenPos(ImVec2(curPos.x + 10 * mContext.state.mainScale, curPos.y + 42 * mContext.state.mainScale));
		ImGui::BeginChild("##MasterVolParam", ImVec2(width - 50 * mContext.state.mainScale, 35 * mContext.state.mainScale), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
		master->GetVolumeParameter()->Draw();
		ImGui::EndChild();

		float peakL = master->GetPeakL();
		float peakR = master->GetPeakR();
		float meterW = 10.0f * mContext.state.mainScale;
		float meterH = masterHeight - 10.0f;
		float meterX = curPos.x + width - 30.0f * mContext.state.mainScale;
		float meterY = curPos.y + 5.0f;

		auto LinToNorm = [](float val) -> float {
			if (val <= 0.0001f)
				return 0.0f;
			float db = 20.0f * std::log10(val);
			float norm = (db + 60.0f) / 60.0f;
			return std::clamp(norm, 0.0f, 1.0f);
		};

		float hL = LinToNorm(peakL) * meterH;
		float hR = LinToNorm(peakR) * meterH;
		float normL = LinToNorm(peakL);
		float normR = LinToNorm(peakR);

		drawList->AddRectFilled(ImVec2(meterX, meterY), ImVec2(meterX + meterW, meterY + meterH), th.meterBg);
		drawList->AddRectFilled(ImVec2(meterX, meterY + meterH - hL), ImVec2(meterX + meterW * 0.5f, meterY + meterH), th.MeterColor(normL));
		drawList->AddRectFilled(ImVec2(meterX + meterW * 0.5f, meterY + meterH - hR), ImVec2(meterX + meterW, meterY + meterH), th.MeterColor(normR));
	}
}
