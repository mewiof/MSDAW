#include "PrecompHeader.h"
#include "TrackListView.h"
#include "AppConfig.h"
#include "Project.h"
#include "ProcessorFactory.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"
#include "Undo/Actions.h"
#include "Theme.h"
#include "TimelineView/TrackLayout.h"
#include <algorithm>
#include <filesystem>
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

	float headerHeight = 34 * mContext.state.mainScale; // match TimelineView's rulerHeight so the header strip lines up with the ruler

	auto& tracks = project->GetTracks();

	// per-track vertical bands (variable height: collapsed tracks shrink, folded groups hide children)
	auto rows = TrackLayout::Build(mContext);

	float txtH = ImGui::GetTextLineHeight();

	// draws a fold/collapse caret (down when expanded, right when collapsed) as an
	// overlapping button; returns true on click
	auto DrawCaret = [&](const char* id, float x, float y, float w, float h, bool open) -> bool {
		ImGui::SetCursorScreenPos(ImVec2(x, y));
		ImGui::SetNextItemAllowOverlap();
		bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
		ImU32 col = ImGui::IsItemHovered() ? th.text : th.textMuted;
		float cx = x + w * 0.5f;
		float cy = y + h * 0.5f;
		float r = h * 0.28f;
		if (open)
			drawList->AddTriangleFilled(ImVec2(cx - r, cy - r * 0.6f), ImVec2(cx + r, cy - r * 0.6f), ImVec2(cx, cy + r * 0.7f), col);
		else
			drawList->AddTriangleFilled(ImVec2(cx - r * 0.6f, cy - r), ImVec2(cx - r * 0.6f, cy + r), ImVec2(cx + r * 0.7f, cy), col);
		return clicked;
	};

	// the M/S pair, drawn the same way on a full row and on a minimized one: a
	// collapsed track still has to be silenceable without expanding it again
	auto DrawMuteSolo = [&](const std::shared_ptr<Track>& track) {
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
			// ctrl adds this track to whatever is already soloed; a plain click is
			// exclusive, and clicking the one soloed track clears the solo entirely
			if (ImGui::GetIO().KeyCtrl) {
				track->SetSolo(!solo);
			} else {
				bool wasSolo = solo;
				for (auto& t : project->GetTracks())
					t->SetSolo(false);
				if (!wasSolo)
					track->SetSolo(true);
			}
		}
		if (solo)
			ImGui::PopStyleColor();
	};

	// how wide that pair comes out, so a minimized row can reserve the space before
	// deciding where its name has to stop
	const float muteSoloWidth = ImGui::CalcTextSize("M").x + ImGui::CalcTextSize("S").x +
								ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x;

	auto LinToNorm = [](float val) -> float {
		if (val <= 0.0001f)
			return 0.0f;
		float db = 20.0f * std::log10(val);
		float norm = (db + 60.0f) / 60.0f;
		return std::clamp(norm, 0.0f, 1.0f);
	};

	// ---- full-height row layout, shared by the tracks and the master ----
	// the master is laid out by the same code as the tracks above it, so its name, its
	// buttons and its faders sit in the same columns rather than in a near-copy that
	// drifts out of line with them
	const float s = mContext.state.mainScale;
	const float frmH = ImGui::GetFrameHeight();
	const float meterW = 6 * s;
	const float meterX = fixedPos.x + width - meterW - 4 * s;

	// the name, the button row and the mixer row, top to bottom. the row's spare height
	// is shared evenly above, between and below them, so the stack sits centered in the
	// row at any font size instead of hanging off fixed offsets from its top
	struct RowBands {
		float yName;
		float yButtons;
		float yMixer;
	};
	auto LayoutBands = [&](float rowTop, float rowH) -> RowBands {
		const float gap = std::max(rowH - txtH * 2.0f - frmH, 0.0f) * 0.25f;
		return {rowTop + gap, rowTop + gap * 2.0f + txtH, rowTop + gap * 3.0f + txtH * 2.0f};
	};

	// volume + pan on one compact row: an inline label plus a single-height value box
	// (with a level fill), sized to fill the span between the content column and the meter
	auto DrawMixer = [&](Track* t, float contentX, float yMixer) {
		const float mixerW = (meterX - 6 * s) - contentX;
		const float mixerGap = 8 * s;
		const float mixerHalfW = (mixerW - mixerGap) * 0.5f;
		const float labelY = yMixer + (frmH - txtH) * 0.5f;

		auto DrawLabeledParam = [&](const char* label, Parameter* p, float x) {
			float labelW = ImGui::CalcTextSize(label).x;
			float sliderW = std::max(mixerHalfW - labelW - 4 * s, 20 * s);
			drawList->AddText(ImVec2(x, labelY), th.textMuted, label);
			ImGui::SetCursorScreenPos(ImVec2(x + labelW + 4 * s, yMixer));
			p->DrawCompact(sliderW, "%.2f", true);
		};

		DrawLabeledParam("Vol", t->GetVolumeParameter(), contentX);
		DrawLabeledParam("Pan", t->GetPanParameter(), contentX + mixerHalfW + mixerGap);
	};

	// a stereo peak meter down the row's right edge
	auto DrawMeter = [&](Track* t, float meterTop, float meterH) {
		float normL = LinToNorm(t->GetPeakL());
		float normR = LinToNorm(t->GetPeakR());
		drawList->AddRectFilled(ImVec2(meterX, meterTop), ImVec2(meterX + meterW, meterTop + meterH), th.meterBg);
		drawList->AddRectFilled(ImVec2(meterX, meterTop + meterH - normL * meterH), ImVec2(meterX + meterW * 0.5f, meterTop + meterH), th.MeterColor(normL));
		drawList->AddRectFilled(ImVec2(meterX + meterW * 0.5f, meterTop + meterH - normR * meterH), ImVec2(meterX + meterW, meterTop + meterH), th.MeterColor(normR));
	};

	int trackToProcess = -1;
	enum Action { None,
				  Delete,
				  Ungroup,
				  AddAfter,
				  RenderToNew };
	Action action = None;

	ImVec2 clipMin(fixedPos.x, stickyY + headerHeight);
	ImVec2 clipMax(fixedPos.x + width, masterY);
	ImGui::PushClipRect(clipMin, clipMax, true);

	for (size_t i = 0; i < tracks.size(); ++i) {
		ImGui::PushID((int)i);
		auto track = tracks[i];

		// skip tracks hidden inside a folded group
		if (!rows[i].visible) {
			ImGui::PopID();
			continue;
		}

		float rowH = rows[i].height;
		float curY = trackAreaStartY + rows[i].top;

		if (curY + rowH < stickyY + headerHeight || curY > masterY) {
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

		float indent = 0.0f;
		if (track->GetParent()) {
			indent = 15.0f * s;
			auto p = track->GetParent();
			while (p->GetParent()) {
				indent += 15.0f * s;
				p = p->GetParent();
			}
		}

		bool minimized = track->mIsCollapsed && !track->IsGroup();

		// ---- shared row surface: background, colour strip, selection + drag/drop ----
		ImGui::SetCursorScreenPos(ImVec2(fixedPos.x + indent, curY));
		ImGui::SetNextItemAllowOverlap();

		float selW = width - indent - (minimized ? (muteSoloWidth + meterW + 14 * s) : 45 * s);
		selW = std::max(selW, 12 * s);
		if (ImGui::InvisibleButton("TrackSelect", ImVec2(selW, rowH))) {
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
				mContext.state.SelectTrack((int)i);
			}
		}

		// NOTE: the row surface is painted HERE, below the selection button rather than
		// above it. the button draws nothing of its own, so the fill still lands under
		// everything on the row - but the selection it reads is this frame's, so a
		// clicked row lights up immediately instead of one frame later
		bool isTrackSelected = (mContext.state.selectedTrackIndex == (int)i) ||
							   mContext.state.multiSelectedTracks.count((int)i) > 0;
		ImU32 bgCol = isTrackSelected ? th.bgActive : th.bgPanel;
		if (track->IsGroup())
			bgCol = isTrackSelected ? th.bgActive : th.bgPanelAlt; // groups read as slightly raised containers

		drawList->AddRectFilled(ImVec2(fixedPos.x + indent, curY), ImVec2(fixedPos.x + width, curY + rowH), bgCol);
		drawList->AddRectFilled(ImVec2(fixedPos.x + indent, curY), ImVec2(fixedPos.x + indent + 6 * s, curY + rowH), track->GetColor());

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
			if (ImGui::MenuItem("Add Track After This One")) {
				trackToProcess = (int)i;
				action = AddAfter;
			}
			// the label says which of the two ranges the bounce will use, because from a
			// context menu there is nothing else to tell the user a selection is in play
			const bool hasTimeSelection = mContext.state.selectionEnd > mContext.state.selectionStart;
			if (ImGui::MenuItem(hasTimeSelection ? "Render Selection to New Track" : "Render to New Track")) {
				trackToProcess = (int)i;
				action = RenderToNew;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Bounce this track through its devices onto a new track below it");
			ImGui::Separator();
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
			// fold (groups) / minimize (tracks) toggle
			const char* collapseLabel = track->IsGroup()
											? (track->mIsCollapsed ? "Unfold" : "Fold")
											: (track->mIsCollapsed ? "Restore Height" : "Minimize");
			if (ImGui::MenuItem(collapseLabel))
				track->mIsCollapsed = !track->mIsCollapsed;
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

		// ---- compact (minimized) row: caret + name + small meter ----
		if (minimized) {
			float caretX = fixedPos.x + indent + 6 * s;
			float caretW = 14 * s;
			float rowCenterY = curY + (rowH - txtH) * 0.5f;
			if (DrawCaret("##Caret", caretX, rowCenterY, caretW, txtH, !track->mIsCollapsed))
				track->mIsCollapsed = !track->mIsCollapsed;

			float buttonsX = meterX - 6 * s - muteSoloWidth;

			float nameX = caretX + caretW + 2 * s;
			ImGui::PushClipRect(ImVec2(nameX, curY), ImVec2(buttonsX - 4 * s, curY + rowH), true);
			drawList->AddText(ImVec2(nameX, rowCenterY), th.text, track->GetName().c_str());
			ImGui::PopClipRect();

			ImGui::SetCursorScreenPos(ImVec2(buttonsX, rowCenterY));
			DrawMuteSolo(track);

			// slim level meter on the right edge
			DrawMeter(track.get(), curY + 3 * s, rowH - 6 * s);

			ImGui::PopID();
			continue;
		}

		// ---- full row: name, buttons and mixer stacked inside the row ----
		const RowBands bands = LayoutBands(curY, rowH);
		const float yName = bands.yName;

		float contentX = fixedPos.x + indent + 22 * s; // right of the colour strip + caret column

		// close button at the end of the name line, just left of the meter
		float closeSz = 15 * s;
		float closeX = meterX - closeSz - 6 * s;
		float closeY = yName + (txtH - closeSz) * 0.5f;

		// fold/minimize caret in the reserved column left of the name
		if (DrawCaret("##Caret", fixedPos.x + indent + 6 * s, yName, 14 * s, txtH, !track->mIsCollapsed))
			track->mIsCollapsed = !track->mIsCollapsed;

		ImGui::SetCursorScreenPos(ImVec2(contentX, yName));

		if (mRenamingIndex == (int)i) {
			ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(120 * s);
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
			// clip the name to the space before the close button so it never overlaps it
			ImGui::PushClipRect(ImVec2(contentX, yName), ImVec2(closeX - 4 * s, yName + txtH + 2 * s), true);
			ImGui::Text("%s", dispName.c_str());
			ImGui::PopClipRect();

			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
				mRenamingIndex = (int)i;
				strcpy(mRenameBuf, track->GetName().c_str());
			}
		}

		ImGui::SetCursorScreenPos(ImVec2(contentX, bands.yButtons));

		DrawMuteSolo(track);

		ImGui::SameLine();

		bool showAuto = track->mShowAutomation;
		if (showAuto)
			ImGui::PushStyleColor(ImGuiCol_Button, th.danger);
		if (ImGui::SmallButton("A"))
			track->mShowAutomation = !showAuto;
		if (showAuto)
			ImGui::PopStyleColor();

		DrawMixer(track.get(), contentX, bands.yMixer);
		DrawMeter(track.get(), curY + 4 * s, rowH - 8 * s);

		ImGui::SetCursorScreenPos(ImVec2(closeX, closeY));
		if (ImGui::Button("X", ImVec2(closeSz, closeSz))) {
			trackToProcess = (int)i;
			action = Delete;
		}

		ImGui::PopID();
	}

	ImGui::SetCursorScreenPos(ImVec2(fixedPos.x, trackAreaStartY + TrackLayout::TotalHeight(rows)));
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

	// the empty space under the last track: the only place left to ask for a track when
	// the list is empty, which is exactly when there is no row to right-click
	const float emptyTop = trackAreaStartY + TrackLayout::TotalHeight(rows) + 10.0f;
	const float emptyHeight = masterY - emptyTop;
	if (emptyHeight > 1.0f) {
		ImGui::SetCursorScreenPos(ImVec2(fixedPos.x, emptyTop));
		ImGui::InvisibleButton("##TrackListEmpty", ImVec2(width, emptyHeight), ImGuiButtonFlags_MouseButtonRight);
		if (ImGui::BeginPopupContextItem()) {
			if (ImGui::MenuItem("Add Track")) {
				TrackTopologyAction::Record(mContext.undoManager, project, "Add track", [&] {
					project->CreateTrack();
				});
			}
			ImGui::EndPopup();
		}
	}

	ImGui::PopClipRect();

	drawList->AddRectFilled(ImVec2(fixedPos.x, stickyY), ImVec2(fixedPos.x + width, stickyY + headerHeight), th.bgHeader);
	drawList->AddLine(ImVec2(fixedPos.x, stickyY + headerHeight), ImVec2(fixedPos.x + width, stickyY + headerHeight), th.borderStrong);

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
		} else if (action == AddAfter) {
			TrackTopologyAction::Record(mContext.undoManager, project, "Add track", [&] {
				project->CreateTrackAfter(trackToProcess);
			});
		} else if (action == RenderToNew) {
			// the wav belongs with the project that points at it. a project that has
			// never been saved has no folder of its own, so those land in the app's
			// until it does
			std::filesystem::path renderDirectory = mContext.state.projectPath.empty()
														? std::filesystem::path(AppConfig::DataDirectory()) / "Rendered"
														: std::filesystem::path(mContext.state.projectPath).parent_path() / "Rendered";

			// one topology step covers the whole thing: the clip rides on the new track
			// object, so restoring the track list restores the bounce with it
			TrackTopologyAction::Record(mContext.undoManager, project, "Render track", [&] {
				project->RenderTrackToNewTrack(trackToProcess, renderDirectory.string(),
											   mContext.state.selectionStart, mContext.state.selectionEnd);
			});
		}
	}

	auto master = project->GetMasterTrack();
	if (master) {
		ImVec2 curPos = ImVec2(fixedPos.x, masterY);

		float masterHeight = mContext.layout.trackRowHeight + mContext.layout.trackGap;

		ImGui::SetCursorScreenPos(curPos);
		ImGui::SetNextItemAllowOverlap();

		if (ImGui::InvisibleButton("MasterSelect", ImVec2(width - 40 * mContext.state.mainScale, masterHeight))) {
			// SelectTrack, not a bare index write: the multi-selection set is drawn as
			// part of the same highlight, and leaving it stale kept the previously
			// selected lane lit in the arrangement after picking master
			mContext.state.SelectTrack(-1);
		}

		// painted after the button, so picking master lights it up on this frame
		bool isMasterSelected = (mContext.state.selectedTrackIndex == -1);
		ImU32 bgCol = isMasterSelected ? th.bgActive : th.bgPanelAlt;
		drawList->AddRectFilled(curPos, ImVec2(curPos.x + width, curPos.y + masterHeight), bgCol);

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

		// laid out by the same code as the track rows, in the tracks' name column: the
		// master has no caret, but its name starts where theirs do
		const RowBands bands = LayoutBands(curPos.y, masterHeight);
		const float contentX = curPos.x + 22 * s;

		ImGui::SetCursorScreenPos(ImVec2(contentX, bands.yName));
		ImGui::Text("MASTER");

		ImGui::SetCursorScreenPos(ImVec2(contentX, bands.yButtons));
		bool showAuto = master->mShowAutomation;
		if (showAuto)
			ImGui::PushStyleColor(ImGuiCol_Button, th.danger);
		if (ImGui::SmallButton("A##Master"))
			master->mShowAutomation = !showAuto;
		if (showAuto)
			ImGui::PopStyleColor();

		DrawMixer(master.get(), contentX, bands.yMixer);
		DrawMeter(master.get(), curPos.y + 4 * s, masterHeight - 8 * s);
	}
}
