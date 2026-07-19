#include "PrecompHeader.h"
#include "TimelineView.h"
#include "TrackListView.h"
#include "Project.h"
#include "ProcessorFactory.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"
#include "Undo/Actions.h"
#include <filesystem>
#include <algorithm>
#include <cmath>

#include "TimelineView/TimelineRuler.h"
#include "TimelineView/TimelineTrackView.h"
#include "TimelineView/TimelineAutomationRenderer.h"
#include "Theme.h"

void TimelineView::Render(const ImVec2& pos, float width, float height, TrackListView* trackListView, float trackListW) {
	const Theme& th = Theme::Instance();
	ImGuiIO& io = ImGui::GetIO();
	float timelineWidth = width - trackListW;

	// resolve Ctrl+Wheel zoom BEFORE Begin so the horizontal scrollbar is both sized
	// AND positioned from THIS frame's zoom. imgui derives the grab from the content
	// size + scroll it had at the previous End(), so declaring them up front
	// (SetNextWindowContentSize / SetNextWindowScroll) removes the one-frame lag that
	// made the grab jitter in size and position while zooming
	float zoomOldPPB = mContext.state.pixelsPerBeat;
	bool didZoom = false;
	if (mHoveredLastFrame && io.KeyCtrl && io.MouseWheel != 0.0f) {
		float zoomFactor = io.MouseWheel > 0.0f ? 1.1f : (1.0f / 1.1f);
		mContext.state.pixelsPerBeat = std::clamp(zoomOldPPB * zoomFactor, 10.0f, 5000.0f);
		didZoom = mContext.state.pixelsPerBeat != zoomOldPPB;
	}
	{
		double maxBeatDecl = 100.0;
		if (Project* declProject = mContext.GetProject())
			for (const auto& t : declProject->GetTracks())
				for (const auto& c : t->GetClips())
					maxBeatDecl = std::max(maxBeatDecl, c->GetEndBeat());
		float neededDecl = (float)(maxBeatDecl * mContext.state.pixelsPerBeat);
		float contentWidthDecl = std::max(neededDecl, timelineWidth) + trackListW;
		ImGui::SetNextWindowContentSize(ImVec2(contentWidthDecl, 0.0f)); // 0 y == leave vertical automatic
	}

	// keep the beat under the cursor pinned across the zoom. mContentLeftX is last
	// frame's scroll-independent content origin and state.timelineScrollX is last
	// frame's scroll - together they locate the mouse in content space, letting us
	// re-derive the scroll that holds that beat steady. applied this frame (not next)
	// so the grab position tracks the zoom without lag
	if (didZoom && mContentLeftValid) {
		float newPPB = mContext.state.pixelsPerBeat;
		float mouseX = io.MousePos.x;
		float contentLeft0 = mContentLeftX;
		double mouseBeat = (double)(mouseX - (contentLeft0 - mContext.state.timelineScrollX)) / zoomOldPPB;
		float newScrollX = (float)(mouseBeat * newPPB) - (mouseX - contentLeft0);
		if (newScrollX < 0.0f)
			newScrollX = 0.0f;
		ImGui::SetNextWindowScroll(ImVec2(newScrollX, -1.0f)); // -1 y == leave vertical scroll untouched
	}

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::Begin("Arrangement", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleVar();

	mHoveredLastFrame = ImGui::IsWindowHovered();

	Project* project = mContext.GetProject();
	Transport* transport = project ? &project->GetTransport() : nullptr;

	// follow-playback scrolling runs after Begin and reads the post-zoom
	// pixelsPerBeat (the zoom is resolved before Begin, above), so the two never
	// fight over the scroll on the same frame

	if (ImGui::IsWindowHovered() && (ImGui::GetIO().MouseWheelH != 0.0f || (ImGui::GetIO().MouseWheel != 0.0f && ImGui::GetIO().KeyShift))) {
		mContext.state.followPlayback = false;
	}

	if (!mContext.state.restoreScroll) {
		mContext.state.timelineScrollX = ImGui::GetScrollX();
		mContext.state.timelineScrollY = ImGui::GetScrollY();
	}

	if (project) {
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 winPos = ImGui::GetCursorScreenPos(); // current position relative to scroll
		float scrollX = ImGui::GetScrollX();
		auto& tracks = project->GetTracks();

		// cache the scroll-independent content origin (stable while the panel does not
		// move) for next frame's pre-Begin zoom anchor. captured from the pristine
		// winPos, before the follow-playback block below shifts it
		mContentLeftX = winPos.x + scrollX;
		mContentLeftValid = true;

		// sample the transport position once per frame so the follow scroll and the
		// playhead line are computed from the exact same beat (the audio thread keeps
		// advancing GetPosition(), and reading it twice would offset the two by a few
		// samples -> visible cursor jitter / doubling when zoomed in)
		double playbackBeat = transport
								  ? (double)transport->GetPosition() / transport->GetSampleRate() * (transport->GetBpm() / 60.0)
								  : 0.0;

		// handle keyboard shortcuts
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
			if (io.KeyCtrl) {
				if (ImGui::IsKeyPressed(ImGuiKey_E) && mContext.state.selectedClip) {
					int trackIdx = mContext.state.selectedTrackIndex;
					double splitBeat = mContext.state.selectionStart;

					if (trackIdx >= 0 && trackIdx < (int)tracks.size()) {
						auto clip = mContext.state.selectedClip;
						double start = clip->GetStartBeat();
						double end = clip->GetEndBeat();

						if (splitBeat > start + 0.001 && splitBeat < end - 0.001) {
							auto newClip = CloneClip(clip);

							if (newClip) {
								double splitPointDelta = splitBeat - start;
								clip->SetDuration(splitPointDelta);
								newClip->SetStartBeat(splitBeat);
								newClip->SetDuration(end - splitBeat);
								newClip->SetOffset(clip->GetOffset() + splitPointDelta);

								tracks[trackIdx]->AddClip(newClip);
								mContext.state.selectedClip = newClip;
							}
						}
					}
				}
				if (ImGui::IsKeyPressed(ImGuiKey_C) && mContext.state.selectedClip) {
					mInteraction.clipboard = CloneClip(mContext.state.selectedClip);
				}
				if (ImGui::IsKeyPressed(ImGuiKey_V) && mInteraction.clipboard) {
					int trackIdx = mContext.state.selectedTrackIndex;
					if (trackIdx >= 0 && trackIdx < (int)tracks.size() && !tracks[trackIdx]->mShowAutomation) {
						auto newClip = CloneClip(mInteraction.clipboard);
						if (newClip) {
							double currentBeat = (double)transport->GetPosition() / transport->GetSampleRate() * (transport->GetBpm() / 60.0);
							if (mContext.state.timelineGrid > 0.0)
								currentBeat = round(currentBeat / mContext.state.timelineGrid) * mContext.state.timelineGrid;
							newClip->SetStartBeat(currentBeat);
							tracks[trackIdx]->AddClip(newClip);
							mContext.state.selectedClip = newClip;
						}
					}
				}
				if (ImGui::IsKeyPressed(ImGuiKey_D) && mContext.state.selectedClip) {
					int trackIdx = mContext.state.selectedTrackIndex;
					if (trackIdx >= 0 && trackIdx < (int)tracks.size()) {
						auto newClip = CloneClip(mContext.state.selectedClip);
						if (newClip) {
							double endBeat = mContext.state.selectedClip->GetEndBeat();
							newClip->SetStartBeat(endBeat);
							tracks[trackIdx]->AddClip(newClip);
							mContext.state.selectedClip = newClip;
						}
					}
				}
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Delete) && mContext.state.selectedClip) {
				int selTrackIdx = mContext.state.selectedTrackIndex;
				if (selTrackIdx >= 0 && selTrackIdx < (int)tracks.size()) {
					tracks[selTrackIdx]->RemoveClip(mContext.state.selectedClip);
					mContext.state.selectedClip = nullptr;
				}
			}
		}

		// NOTE: the zoom (pixelsPerBeat, content size and anchored scroll) is fully
		// resolved before Begin now - see the top of Render. nothing to do here

		// follow playback logic (uses the post-zoom pixelsPerBeat / scrollX, so
		// zooming while following keeps the playhead pinned instead of jerking)
		if (transport && transport->IsPlaying() && mContext.state.followPlayback) {
			float playheadX = (float)(playbackBeat * mContext.state.pixelsPerBeat);

			float visibleWidth = ImGui::GetContentRegionAvail().x - trackListW;
			if (visibleWidth <= 0.0f)
				visibleWidth = width - trackListW;

			float targetScroll = scrollX;
			if (mContext.state.followMode == FollowMode::Page) {
				if (playheadX >= scrollX + visibleWidth) {
					targetScroll = scrollX + visibleWidth;
				} else if (playheadX < scrollX) {
					targetScroll = std::floor(playheadX / visibleWidth) * visibleWidth;
				}
			} else {
				targetScroll = playheadX - (visibleWidth * 0.5f);
			}

			if (targetScroll < 0.0f)
				targetScroll = 0.0f;

			// apply the new scroll to this frame's winPos immediately (mirroring the
			// zoom block above). ImGui::SetScrollX only takes effect next frame, so
			// without this the content lags the playhead by one frame and the red
			// cursor jitters / appears doubled when zoomed in
			if (targetScroll != scrollX) {
				ImGui::SetScrollX(targetScroll);
				winPos.x -= (targetScroll - scrollX);
				scrollX = targetScroll;
			}
		}

		float rulerHeight = 34.0f * mContext.state.mainScale;
		float trackAreaStartY = winPos.y + rulerHeight;
		float rowFullHeight = mContext.layout.trackRowHeight + mContext.layout.trackGap;
		float fullContentHeight = tracks.size() * rowFullHeight + 200.0f;

		// dynamic content width calculation
		double maxBeat = 100.0;
		for (const auto& t : tracks) {
			for (const auto& c : t->GetClips()) {
				double end = c->GetEndBeat();
				if (end > maxBeat)
					maxBeat = end;
			}
		}

		float neededWidthPixel = (float)(maxBeat * mContext.state.pixelsPerBeat);
		float contentWidth = std::max(neededWidthPixel, timelineWidth) + trackListW;

		ImGui::SetCursorScreenPos(winPos);
		ImGui::Dummy(ImVec2(contentWidth, fullContentHeight));

		// 1. empty area drag & drop
		ImGui::SetCursorScreenPos(ImVec2(winPos.x, trackAreaStartY));
		ImGui::Dummy(ImVec2(contentWidth, fullContentHeight)); // reserves area in layout

		ImGui::SetCursorScreenPos(ImVec2(winPos.x + scrollX, trackAreaStartY));
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##EmptyTimelineDrop", ImVec2(timelineWidth, fullContentHeight));

		if (ImGui::BeginDragDropTarget()) {
			auto dropBefore = TrackTopologyAction::Snapshot(project);
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST_PLUGIN")) {
				std::string path = (const char*)payload->Data;
				std::filesystem::path p(path);

				project->CreateTrack();
				auto& allTracks = project->GetTracks();
				if (!allTracks.empty()) {
					auto newTrack = allTracks.back();
					newTrack->SetName(p.stem().string());
					auto vST = std::make_shared<VSTProcessor>(path);
					if (vST->Load()) {
						newTrack->AddProcessor(vST);
						if (project->GetTransport().GetSampleRate() > 0)
							vST->PrepareToPlay(project->GetTransport().GetSampleRate());
					}
					mContext.state.selectedTrackIndex = (int)allTracks.size() - 1;
				}
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST3_PLUGIN")) {
				std::string data = (const char*)payload->Data;
				size_t pipe = data.find('|');
				if (pipe != std::string::npos) {
					std::string path = data.substr(0, pipe);
					std::string classID = data.substr(pipe + 1);
					std::filesystem::path p(path);

					project->CreateTrack();
					auto& allTracks = project->GetTracks();
					if (!allTracks.empty()) {
						auto newTrack = allTracks.back();
						newTrack->SetName(p.stem().string());
						auto vST = std::make_shared<VST3Processor>(path, classID);
						if (vST->Load()) {
							newTrack->AddProcessor(vST);
							if (project->GetTransport().GetSampleRate() > 0)
								vST->PrepareToPlay(project->GetTransport().GetSampleRate());
						}
						mContext.state.selectedTrackIndex = (int)allTracks.size() - 1;
					}
				}
			}
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INTERNAL_PLUGIN")) {
				std::string type = (const char*)payload->Data;
				std::shared_ptr<AudioProcessor> proc = ProcessorFactory::Instance().Create(type);

				if (proc) {
					project->CreateTrack();
					auto& allTracks = project->GetTracks();
					if (!allTracks.empty()) {
						auto newTrack = allTracks.back();
						newTrack->SetName(type);
						newTrack->AddProcessor(proc);
						if (project->GetTransport().GetSampleRate() > 0)
							proc->PrepareToPlay(project->GetTransport().GetSampleRate());
						mContext.state.selectedTrackIndex = (int)allTracks.size() - 1;
					}
				}
			}
			auto dropAfter = TrackTopologyAction::Snapshot(project);
			if (dropAfter.size() != dropBefore.size())
				mContext.undoManager.Push(std::make_unique<TrackTopologyAction>(project, dropBefore, dropAfter, "Add track"));
			ImGui::EndDragDropTarget();
		}

		ImGui::SetCursorScreenPos(winPos);

		// 2. file drop logic
		if (mContext.state.processDrop) {
			bool insideTimeline = (mContext.state.dropX >= pos.x && mContext.state.dropX <= pos.x + timelineWidth &&
								   mContext.state.dropY >= pos.y && mContext.state.dropY <= pos.y + height);

			if (insideTimeline) {
				float relY = mContext.state.dropY - trackAreaStartY;
				int trackIndex = (int)(relY / rowFullHeight);
				float relX = mContext.state.dropX - winPos.x;
				double startBeat = (double)relX / mContext.state.pixelsPerBeat;
				if (startBeat < 0)
					startBeat = 0;
				if (mContext.state.timelineGrid > 0.0)
					startBeat = round(startBeat / mContext.state.timelineGrid) * mContext.state.timelineGrid;

				std::filesystem::path p(mContext.state.droppedPath);
				std::string ext = p.extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
				bool handled = false;

				if ((ext == ".wav" || ext == ".mp3" || ext == ".flac") && trackIndex >= 0 && trackIndex < (int)tracks.size()) {
					if (!tracks[trackIndex]->mShowAutomation) {
						auto clip = std::make_shared<AudioClip>();
						clip->SetName(p.filename().string());
						if (clip->LoadFromFile(mContext.state.droppedPath)) {
							// calculate proper clip duration based on sample rate and bpm
							double sampleRate = clip->GetSampleRate();
							uint64_t frames = clip->GetTotalFileFrames();
							double projectBpm = transport ? transport->GetBpm() : 120.0;

							if (sampleRate > 0) {
								double durationSecs = (double)frames / sampleRate;
								double durationBeats = durationSecs * (projectBpm / 60.0);
								clip->SetDuration(durationBeats);
							}

							clip->SetStartBeat(startBeat);
							tracks[trackIndex]->AddClip(clip);
							handled = true;
						}
					}
				} else if ((ext == ".mid" || ext == ".mIDI") && trackIndex >= 0 && trackIndex < (int)tracks.size()) {
					if (!tracks[trackIndex]->mShowAutomation) {
						auto clip = std::make_shared<MIDIClip>();
						clip->SetName(p.filename().string());
						if (clip->LoadFromFile(mContext.state.droppedPath)) {
							clip->SetStartBeat(startBeat);
							tracks[trackIndex]->AddClip(clip);
							handled = true;
						}
					}
				}
				if (handled)
					mContext.state.processDrop = false;
			}
		}

		// 3. tracks
		PendingClipMove pendingMove;
		PendingClipDelete pendingDelete;

		TimelineTrackView::RenderTracks(mContext, mInteraction, pendingMove, pendingDelete, winPos, contentWidth - trackListW, timelineWidth, scrollX, trackAreaStartY);

		if (pendingMove.valid) {
			auto fromTrack = tracks[pendingMove.fromTrackIdx];
			auto toTrack = tracks[pendingMove.toTrackIdx];
			auto fromBefore = ClipSnapshotAction::Snapshot(fromTrack);
			auto toBefore = ClipSnapshotAction::Snapshot(toTrack);
			fromTrack->RemoveClip(pendingMove.clip);
			pendingMove.clip->SetStartBeat(pendingMove.newStartBeat);
			toTrack->AddClip(pendingMove.clip);
			mContext.state.selectedTrackIndex = pendingMove.toTrackIdx;
			// record both tracks' clip changes as one undo step
			mContext.undoManager.BeginTransaction("Move clip");
			mContext.undoManager.Push(std::make_unique<ClipSnapshotAction>(project, fromTrack, fromBefore, ClipSnapshotAction::Snapshot(fromTrack), "Move clip"));
			mContext.undoManager.Push(std::make_unique<ClipSnapshotAction>(project, toTrack, toBefore, ClipSnapshotAction::Snapshot(toTrack), "Move clip"));
			mContext.undoManager.EndTransaction();
		}
		if (pendingDelete.valid) {
			auto delTrack = tracks[pendingDelete.trackIdx];
			auto before = ClipSnapshotAction::Snapshot(delTrack);
			delTrack->RemoveClip(pendingDelete.clip);
			mContext.undoManager.Push(std::make_unique<ClipSnapshotAction>(project, delTrack, before, ClipSnapshotAction::Snapshot(delTrack), "Delete clip"));
			if (mContext.state.selectedClip == pendingDelete.clip)
				mContext.state.selectedClip = nullptr;
		}

		// preview os drag & drop
		if (mContext.state.isOsDragging) {
			bool insideTimeline = (mContext.state.osDragX >= pos.x && mContext.state.osDragX <= pos.x + timelineWidth &&
								   mContext.state.osDragY >= pos.y && mContext.state.osDragY <= pos.y + height);

			if (insideTimeline) {
				float relY = mContext.state.osDragY - trackAreaStartY;
				int trackIndex = (int)(relY / rowFullHeight);

				float relX = mContext.state.osDragX - winPos.x;
				double startBeat = (double)relX / mContext.state.pixelsPerBeat;
				if (startBeat < 0)
					startBeat = 0;
				if (mContext.state.timelineGrid > 0.0)
					startBeat = round(startBeat / mContext.state.timelineGrid) * mContext.state.timelineGrid;

				if (trackIndex >= 0 && trackIndex < (int)tracks.size()) {
					// draw insertion marker (instead of fake box)
					float ghostX = winPos.x + (float)(startBeat * mContext.state.pixelsPerBeat);
					float ghostY = trackAreaStartY + (trackIndex * rowFullHeight);
					float ghostH = mContext.layout.trackRowHeight;

					// vertical line
					drawList->AddLine(ImVec2(ghostX, ghostY), ImVec2(ghostX, ghostY + ghostH), th.dropLine, 4.0f);

					// label
					const char* label = "Insert File";
					ImVec2 textSize = ImGui::CalcTextSize(label);
					ImVec2 boxMin(ghostX + 5, ghostY + 5);
					ImVec2 boxMax(ghostX + 5 + textSize.x + 4, ghostY + 5 + textSize.y + 4);
					drawList->AddRectFilled(boxMin, boxMax, th.bgOverlay, 2.0f);
					drawList->AddText(ImVec2(ghostX + 7, ghostY + 7), th.text, label);
				}
			}
		}

		// renaming
		if (mInteraction.triggerRenamePopup) {
			ImGui::OpenPopup("Rename Clip");
			mInteraction.triggerRenamePopup = false;
		}
		if (ImGui::BeginPopupModal("Rename Clip", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("Enter new name:");

			// auto focus
			if (ImGui::IsWindowAppearing())
				ImGui::SetKeyboardFocusHere(0);

			bool enterPressed = ImGui::InputText("##Name", mInteraction.renameBuffer, sizeof(mInteraction.renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
			ImGui::Separator();

			if (ImGui::Button("Set", ImVec2(120, 0)) || enterPressed) {
				if (mInteraction.clipToRename)
					mInteraction.clipToRename->SetName(mInteraction.renameBuffer);
				mInteraction.clipToRename = nullptr;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				mInteraction.clipToRename = nullptr;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		auto master = project->GetMasterTrack();
		float masterHeight = mContext.layout.trackRowHeight + mContext.layout.trackGap;
		float masterPadding = (master && master->mShowAutomation) ? masterHeight + 20.0f : 20.0f;
		float hScrollbarSize = ImGui::GetStyle().ScrollbarSize;
		float masterY = pos.y + height - masterHeight - hScrollbarSize;

		ImGui::SetCursorScreenPos(ImVec2(winPos.x, trackAreaStartY + tracks.size() * rowFullHeight));
		ImGui::Dummy(ImVec2(contentWidth, masterPadding));

		if (master && master->mShowAutomation) {
			drawList->AddRectFilled(ImVec2(winPos.x, masterY), ImVec2(winPos.x + contentWidth - trackListW, masterY + masterHeight), th.bgWindow);
			drawList->AddRect(ImVec2(winPos.x, masterY), ImVec2(winPos.x + contentWidth - trackListW, masterY + masterHeight), th.border);

			TimelineAutomationRenderer::Render(mContext, mInteraction, master.get(), -1, winPos, contentWidth - trackListW, timelineWidth, scrollX, masterY);
		}

		float stickyY = winPos.y + ImGui::GetScrollY();
		ImVec2 stickyPos = ImVec2(winPos.x, stickyY);

		if (mContext.state.selectionEnd > mContext.state.selectionStart) {
			float selX1 = winPos.x + (float)(mContext.state.selectionStart * mContext.state.pixelsPerBeat);
			float selX2 = winPos.x + (float)(mContext.state.selectionEnd * mContext.state.pixelsPerBeat);
			drawList->AddRectFilled(ImVec2(selX1, trackAreaStartY), ImVec2(selX2, stickyY + height), th.selectionFill);
		}

		TimelineRuler::Render(mContext, mInteraction, stickyPos, contentWidth - trackListW, timelineWidth, rulerHeight, scrollX);

		// insert marker
		float insertX = winPos.x + (float)(mContext.state.selectionStart * mContext.state.pixelsPerBeat);
		if (insertX >= pos.x && insertX <= pos.x + timelineWidth) {
			drawList->AddLine(ImVec2(insertX, stickyY), ImVec2(insertX, stickyY + height), Theme::WithAlpha(th.marker, 200), 1.0f);
			drawList->AddTriangleFilled(ImVec2(insertX - 4, stickyY), ImVec2(insertX + 4, stickyY), ImVec2(insertX, stickyY + 6), th.marker);
		}

		// playhead
		if (transport) {
			float playheadX = winPos.x + (float)(playbackBeat * mContext.state.pixelsPerBeat);

			if (playheadX >= pos.x - 2.0f && playheadX <= pos.x + timelineWidth + 2.0f) {
				drawList->AddLine(ImVec2(playheadX, stickyY), ImVec2(playheadX, stickyY + height), th.playhead, 1.5f);
				drawList->AddTriangleFilled(ImVec2(playheadX - 6, stickyY), ImVec2(playheadX + 6, stickyY), ImVec2(playheadX, stickyY + 10), th.playhead);
			}
		}

		if (mContext.state.restoreScroll) {
			ImGui::SetScrollX(mContext.state.timelineScrollX);
			ImGui::SetScrollY(mContext.state.timelineScrollY);
			mContext.state.restoreScroll = false;
		}

		if (trackListView) {
			float trackListX = pos.x + timelineWidth;
			ImVec2 trackListFixedPos(trackListX, pos.y);

			drawList->AddRectFilled(
				ImVec2(trackListFixedPos.x, pos.y),
				ImVec2(trackListFixedPos.x + trackListW, pos.y + height - hScrollbarSize),
				th.bgHeader);
			drawList->AddLine(
				ImVec2(trackListFixedPos.x, pos.y),
				ImVec2(trackListFixedPos.x, pos.y + height - hScrollbarSize),
				th.divider, 2.0f);

			trackListView->Render(trackListFixedPos, trackListW, height, trackAreaStartY, stickyY, masterY);
		}
	}
	ImGui::End();
}
