#include "PrecompHeader.h"
#include "TrackListView.h"
#include "Project.h"
#include "ProcessorFactory.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"
#include <algorithm>
#include <vector>
#include <cmath>
#include <cstring>
#include <mutex>

struct TrackMovePayload {
	int srcIndex;
};

void TrackListView::Render(const ImVec2& pos, float width, float height) {
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::Begin("Tracks", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

	Project* project = mContext.GetProject();
	if (project) {
		// top bar: add button
		if (ImGui::Button("+ Add Track", ImVec2(width - 16 * mContext.state.mainScale, 30 * mContext.state.mainScale))) {
			project->CreateTrack();
		}
		ImGui::Separator();

		float rowFullHeight = mContext.layout.trackRowHeight + mContext.layout.trackGap;
		float masterHeight = rowFullHeight;
		float hScrollbarSize = ImGui::GetStyle().ScrollbarSize;
		float listHeight = height - (30 * mContext.state.mainScale) - masterHeight - hScrollbarSize;

		ImGui::BeginChild("TrackListContent", ImVec2(0, listHeight), false, ImGuiWindowFlags_None);

		auto& tracks = project->GetTracks();
		ImGui::SetCursorPosY(-mContext.state.timelineScrollY);

		// handle deletion logic after loop
		int trackToProcess = -1;
		enum Action { None,
					  Delete,
					  Ungroup };
		Action action = None;

		// adjust available width for controls to account for potential scrollbar
		float contentWidth = ImGui::GetContentRegionAvail().x;

		for (size_t i = 0; i < tracks.size(); ++i) {
			ImGui::PushID((int)i);
			auto track = tracks[i];

			// drop target (reorder gap)
			// we place a small invisible button before each track to detect drops for reordering
			ImGui::SetCursorPos(ImVec2(0, i * rowFullHeight));
			ImGui::InvisibleButton("##ReorderGap", ImVec2(contentWidth, 6.0f));
			if (ImGui::BeginDragDropTarget()) {
				// 2. dropping internal effect
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TRACK_MOVE")) {
					TrackMovePayload* data = (TrackMovePayload*)payload->Data;
					if (data->srcIndex != (int)i) {
						// move before this index, not as child (reorder in list)
						project->MoveTrack(data->srcIndex, (int)i, false);
					}
				}
				ImGui::EndDragDropTarget();
			}

			// check selection
			bool isTrackSelected = (mContext.state.selectedTrackIndex == (int)i);
			if (mContext.state.multiSelectedTracks.count((int)i))
				isTrackSelected = true;

			// rendering layout
			ImVec2 curPos = ImGui::GetCursorScreenPos();
			curPos.y += 4.0f;

			// indentation for group children
			float indent = 0.0f;
			if (track->GetParent()) {
				// nesting visual depth
				// TODO: could recurse for deeper trees
				indent = 15.0f * mContext.state.mainScale;
				auto p = track->GetParent();
				while (p->GetParent()) {
					indent += 15.0f * mContext.state.mainScale;
					p = p->GetParent();
				}
			}

			// background
			ImU32 bgCol = isTrackSelected ? IM_COL32(65, 65, 70, 255) : IM_COL32(40, 40, 40, 255);
			if (track->IsGroup())
				bgCol = isTrackSelected ? IM_COL32(60, 50, 60, 255) : IM_COL32(50, 40, 50, 255);

			ImGui::GetWindowDrawList()->AddRectFilled(
				ImVec2(curPos.x + indent, curPos.y),
				ImVec2(curPos.x + width, curPos.y + mContext.layout.trackRowHeight),
				bgCol);

			// colored strip on left using track color
			ImU32 stripCol = track->GetColor();
			ImGui::GetWindowDrawList()->AddRectFilled(
				ImVec2(curPos.x + indent, curPos.y),
				ImVec2(curPos.x + indent + 6 * mContext.state.mainScale, curPos.y + mContext.layout.trackRowHeight),
				stripCol);

			// interactions
			ImGui::SetCursorScreenPos(ImVec2(curPos.x + indent, curPos.y));
			ImGui::SetNextItemAllowOverlap();

			// click to select / drag source / drop target for plugins and grouping
			if (ImGui::InvisibleButton("TrackSelect", ImVec2(contentWidth - indent - 45 * mContext.state.mainScale, mContext.layout.trackRowHeight))) {
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

			// drag source
			if (ImGui::BeginDragDropSource()) {
				TrackMovePayload payload;
				payload.srcIndex = (int)i;
				ImGui::SetDragDropPayload("TRACK_MOVE", &payload, sizeof(TrackMovePayload));
				ImGui::Text("Move %s", track->GetName().c_str());
				ImGui::EndDragDropSource();
			}

			// drop target
			if (ImGui::BeginDragDropTarget()) {
				// 1. dropping a track into a group
				if (track->IsGroup()) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TRACK_MOVE")) {
						TrackMovePayload* data = (TrackMovePayload*)payload->Data;
						if (data->srcIndex != (int)i)
							// as child
							project->MoveTrack(data->srcIndex, (int)i, true);
					}
				}

				// 2. dropping a VST from library
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
				// 3. dropping internal effect from library
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

			// context menu
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
						project->GroupSelectedTracks(mContext.state.multiSelectedTracks);
						mContext.state.multiSelectedTracks.clear();
					}
				}
				ImGui::EndPopup();
			}

			// track name (rename logic)
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
				// apply if clicked away
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

			// mixer controls (m/s/a)
			ImGui::SetCursorScreenPos(ImVec2(curPos.x + indent + 15 * mContext.state.mainScale, curPos.y + 25 * mContext.state.mainScale));

			// mute
			bool mute = track->GetMute();
			if (mute)
				ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 100, 0, 255));
			if (ImGui::SmallButton("M"))
				track->SetMute(!mute);
			if (mute)
				ImGui::PopStyleColor();

			ImGui::SameLine();

			// solo
			bool solo = track->GetSolo();
			if (solo)
				ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 200, 50, 255));
			if (ImGui::SmallButton("S")) {
				bool keyMod = ImGui::GetIO().KeyCtrl;

				if (keyMod)
					// 2. additive
					track->SetSolo(!solo);
				else {
					if (solo)
						// 3. already active + no mod: off everywhere
						for (auto& t : project->GetTracks())
							t->SetSolo(false);
					else {
						// 1. inactive + no mod: exclusive solo (this on, others off)
						for (auto& t : project->GetTracks())
							t->SetSolo(false);
						track->SetSolo(true);
					}
				}
			}
			if (solo)
				ImGui::PopStyleColor();

			ImGui::SameLine();

			// automation
			bool showAuto = track->mShowAutomation;
			if (showAuto)
				ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 50, 50, 255));
			if (ImGui::SmallButton("A"))
				track->mShowAutomation = !showAuto;
			if (showAuto)
				ImGui::PopStyleColor();

			// volume slider
			ImGui::SetCursorScreenPos(ImVec2(curPos.x + indent + 15 * mContext.state.mainScale, curPos.y + 45 * mContext.state.mainScale));
			ImGui::SetNextItemWidth(100 * mContext.state.mainScale);
			ImGui::SliderFloat("##Vol", &track->GetVolumeParameter()->value, -60.0f, 6.0f, "%.1f dB");
			// manually invoke
			track->GetVolumeParameter()->HandleCommonInteractions();

			// volume meter (vertical bar)
			// render on the right side of the track header
			float peakL = track->GetPeakL();
			float peakR = track->GetPeakR();
			float meterW = 6.0f * mContext.state.mainScale;
			float meterH = mContext.layout.trackRowHeight - 10.0f;
			float meterX = curPos.x + contentWidth - 35.0f * mContext.state.mainScale;
			float meterY = curPos.y + 5.0f;

			// convert linear peak to normalized height (logarithmic-ish)
			auto LinToNorm = [](float val) -> float {
				if (val <= 0.0001f)
					return 0.0f;
				float db = 20.0f * std::log10(val);
				// range: -60db to 0db (approx)
				float norm = (db + 60.0f) / 60.0f;
				return std::clamp(norm, 0.0f, 1.0f);
			};

			auto GetMeterColor = [](float norm) -> ImU32 {
				float r, g, b = 0.0f;
				if (norm < 0.75f) {
					// green -> yellow
					float t = norm / 0.75f;
					r = t;
					g = 1.0f;
				} else {
					// yellow -> red
					float t = (norm - 0.75f) / 0.25f;
					r = 1.0f;
					g = 1.0f - t;
				}
				return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
			};

			float hL = LinToNorm(peakL) * meterH;
			float hR = LinToNorm(peakR) * meterH;
			float normL = LinToNorm(peakL);
			float normR = LinToNorm(peakR);

			// background
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(meterX, meterY), ImVec2(meterX + meterW, meterY + meterH), IM_COL32(20, 20, 20, 255));
			// l
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(meterX, meterY + meterH - hL), ImVec2(meterX + meterW * 0.5f, meterY + meterH), GetMeterColor(normL));
			// r
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(meterX + meterW * 0.5f, meterY + meterH - hR), ImVec2(meterX + meterW, meterY + meterH), GetMeterColor(normR));

			// delete button
			// position relative to contentwidth, ensuring it fits if scrollbar appears
			ImGui::SetCursorScreenPos(ImVec2(curPos.x + contentWidth - 25 * mContext.state.mainScale, curPos.y + 5 * mContext.state.mainScale));
			if (ImGui::Button("X", ImVec2(16 * mContext.state.mainScale, 16 * mContext.state.mainScale))) {
				trackToProcess = (int)i;
				action = Delete;
			}

			ImGui::SetCursorScreenPos(ImVec2(curPos.x, curPos.y + mContext.layout.trackRowHeight)); // next row
			ImGui::PopID();
		}

		// reorder gap at the very end
		ImGui::SetCursorPos(ImVec2(0, tracks.size() * rowFullHeight));
		ImGui::InvisibleButton("##ReorderGapEnd", ImVec2(contentWidth, 10.0f));
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TRACK_MOVE")) {
				TrackMovePayload* data = (TrackMovePayload*)payload->Data;
				project->MoveTrack(data->srcIndex, (int)tracks.size(), false);
			}
			ImGui::EndDragDropTarget();
		}

		if (trackToProcess != -1) {
			if (action == Delete) {
				project->RemoveTrack(trackToProcess);
				mContext.state.multiSelectedTracks.clear();
				if (mContext.state.selectedTrackIndex >= (int)tracks.size())
					mContext.state.selectedTrackIndex = std::max(0, (int)tracks.size() - 1);
			} else if (action == Ungroup) {
				project->UngroupTrack(trackToProcess);
			}
		}

		ImGui::EndChild();

		float masterY = pos.y + height - masterHeight - hScrollbarSize;
		ImGui::SetCursorScreenPos(ImVec2(pos.x, masterY));

		// master track
		auto master = project->GetMasterTrack();
		if (master) {
			bool isMasterSelected = (mContext.state.selectedTrackIndex == -1);
			ImU32 bgCol = isMasterSelected ? IM_COL32(65, 50, 50, 255) : IM_COL32(50, 30, 30, 255);
			ImVec2 curPos = ImGui::GetCursorScreenPos();

			ImGui::GetWindowDrawList()->AddRectFilled(curPos, ImVec2(curPos.x + width, curPos.y + masterHeight), bgCol);

			ImGui::SetCursorScreenPos(curPos);

			// enable overlap so the button doesn't block volume slider interaction
			ImGui::SetNextItemAllowOverlap();

			// selection
			if (ImGui::InvisibleButton("MasterSelect", ImVec2(width - 40 * mContext.state.mainScale, masterHeight))) {
				mContext.state.selectedTrackIndex = -1; // -1 for master
			}

			// drop target for plugins on master
			if (ImGui::BeginDragDropTarget()) {
				// 1. dropping a VST
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
				// 2. dropping internal effect
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

			// automation toggle
			ImGui::SetCursorScreenPos(ImVec2(curPos.x + 10, curPos.y + 25));
			bool showAuto = master->mShowAutomation;
			if (showAuto)
				ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 50, 50, 255));
			if (ImGui::SmallButton("A##Master"))
				master->mShowAutomation = !showAuto;
			if (showAuto)
				ImGui::PopStyleColor();

			// volume
			ImGui::SetCursorScreenPos(ImVec2(curPos.x + 10, curPos.y + 45));
			ImGui::SetNextItemWidth(120 * mContext.state.mainScale);
			float vol = master->GetVolumeParameter()->value;
			if (ImGui::SliderFloat("##MVol", &vol, -60.0f, 6.0f, "%.1f dB")) {
				master->GetVolumeParameter()->value = vol;
			}
			master->GetVolumeParameter()->HandleCommonInteractions();

			// meter
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

			auto GetMeterColor = [](float norm) -> ImU32 {
				float r, g, b = 0.0f;
				if (norm < 0.75f) {
					// green -> yellow
					float t = norm / 0.75f;
					r = t;
					g = 1.0f;
				} else {
					// yellow -> red
					float t = (norm - 0.75f) / 0.25f;
					r = 1.0f;
					g = 1.0f - t;
				}
				return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
			};

			float hL = LinToNorm(peakL) * meterH;
			float hR = LinToNorm(peakR) * meterH;
			float normL = LinToNorm(peakL);
			float normR = LinToNorm(peakR);

			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(meterX, meterY), ImVec2(meterX + meterW, meterY + meterH), IM_COL32(20, 20, 20, 255));
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(meterX, meterY + meterH - hL), ImVec2(meterX + meterW * 0.5f, meterY + meterH), GetMeterColor(normL));
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(meterX + meterW * 0.5f, meterY + meterH - hR), ImVec2(meterX + meterW, meterY + meterH), GetMeterColor(normR));
		}
	}
	ImGui::End();
}
