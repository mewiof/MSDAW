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
#include <mutex>

#include "TimelineView/TimelineClipOps.h"
#include "TimelineView/TimelineRuler.h"
#include "TimelineView/TimelineTrackView.h"
#include "TimelineView/TimelineAutomationRenderer.h"
#include "TimelineView/TrackLayout.h"
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

	// an undo, a redo or a delete elsewhere can take a selected clip off its track.
	// the selection holds shared_ptrs so nothing dangles, but a clip that is no longer
	// in the arrangement would still be dragged, duplicated and drawn as selected
	TimelineClipOps::PruneSelection(mContext);

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
		// every one of these acts on the whole clip selection, so the single-clip case
		// is just the block case with one member
		bool arrangementFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		bool anyClipSelected = !mContext.state.selectedClips.empty();
		// D deactivates a clip and is also a note on the computer MIDI keyboard. the
		// arrangement takes the key only while it has something to act on, so the
		// keyboard keeps playing everywhere else
		mContext.state.arrangementOwnsLetterKeys = arrangementFocused && anyClipSelected;

		if (arrangementFocused) {
			bool hasSelection = anyClipSelected;

			if (io.KeyCtrl) {
				// split at the insert marker, the same beat the marker line is drawn at
				if (ImGui::IsKeyPressed(ImGuiKey_E) && hasSelection)
					TimelineClipOps::SplitSelectionAt(mContext, mContext.state.selectionStart);
				if (ImGui::IsKeyPressed(ImGuiKey_A))
					TimelineClipOps::SelectAll(mContext);
				if (ImGui::IsKeyPressed(ImGuiKey_C) && hasSelection)
					TimelineClipOps::CopySelection(mContext, mInteraction);
				if (ImGui::IsKeyPressed(ImGuiKey_X) && hasSelection) {
					TimelineClipOps::CopySelection(mContext, mInteraction);
					TimelineClipOps::DeleteSelection(mContext);
				}
				// the block lands with its top-left corner at the insert marker on the
				// selected track, keeping the shape it was copied in
				if (ImGui::IsKeyPressed(ImGuiKey_V) && !mInteraction.clipboard.empty())
					TimelineClipOps::PasteAt(mContext, mInteraction, mContext.state.selectionStart, mContext.state.selectedTrackIndex);
				if (ImGui::IsKeyPressed(ImGuiKey_D) && hasSelection)
					TimelineClipOps::DuplicateSelection(mContext);
			}
			// D activates/deactivates the selected clips
			if (ImGui::IsKeyPressed(ImGuiKey_D) && !io.KeyCtrl && hasSelection)
				TimelineClipOps::ToggleSelectionEnabled(mContext);
			// F2 renames the focused clip, through the same popup the menu entry opens
			if (ImGui::IsKeyPressed(ImGuiKey_F2) && mContext.state.selectedClip) {
				mInteraction.clipToRename = mContext.state.selectedClip;
				strncpy(mInteraction.renameBuffer, mContext.state.selectedClip->GetName().c_str(), sizeof(mInteraction.renameBuffer));
				mInteraction.renameBuffer[sizeof(mInteraction.renameBuffer) - 1] = 0;
				mInteraction.triggerRenamePopup = true;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Delete) && hasSelection)
				TimelineClipOps::DeleteSelection(mContext);
			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
				mContext.state.ClearClipSelection();
		}

		// NOTE: the zoom (pixelsPerBeat, content size and anchored scroll) is fully
		// resolved before Begin now - see the top of Render. nothing to do here

		// follow playback logic (uses the post-zoom pixelsPerBeat / scrollX, so
		// zooming while following keeps the playhead pinned instead of jerking)
		if (transport && transport->IsPlaying() && mContext.state.followPlayback) {
			float playheadX = (float)(playbackBeat * mContext.state.pixelsPerBeat);

			// the visible timeline viewport is exactly timelineWidth wide (beat 0 sits at
			// content x=0, the track-list column is a fixed overlay to the right). do NOT
			// use GetContentRegionAvail() here: the window declares an explicit content
			// size before Begin (for the zoom scrollbar), so it returns the full content
			// width, which pinned targetScroll at 0 and stalled follow at the far left
			float visibleWidth = timelineWidth;

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
		// per-track vertical bands, shared with the track-list column so collapse/fold stay in sync
		auto rows = TrackLayout::Build(mContext);
		float fullContentHeight = TrackLayout::TotalHeight(rows) + 200.0f;

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
					mContext.state.SelectTrack((int)allTracks.size() - 1);
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
						mContext.state.SelectTrack((int)allTracks.size() - 1);
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
						mContext.state.SelectTrack((int)allTracks.size() - 1);
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
				// only accept a drop that lands on an actual track row (not empty space below)
				int trackIndex = (relY >= 0.0f && relY < TrackLayout::TotalHeight(rows)) ? TrackLayout::RowAtY(rows, relY) : -1;
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
					if (tracks[trackIndex]->AcceptsClips()) {
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
					if (tracks[trackIndex]->AcceptsClips()) {
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

		// a drag whose anchor clip scrolled out of view never reaches the commit inside
		// the clip loop - that code sits behind the same culling test as the clip body -
		// so the gesture would stay live and keep drawing ghosts forever. finish it here
		if (mInteraction.dragState != DragState::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			TimelineClipOps::CommitDrag(mContext, mInteraction, pendingMove);
			mInteraction.dragState = DragState::None;
			mInteraction.dragSourceTrackIdx = -1;
			mInteraction.dragTargetTrackIdx = -1;
			mInteraction.dragEntries.clear();
			mInteraction.dragCollapseCandidate = nullptr;
			mInteraction.dragMoved = false;
		}

		if (pendingMove.valid) {
			// the whole block is lifted out of its lanes before any of it is put back:
			// a clip landing where another moving clip still sits would otherwise be
			// trimmed against a position that is about to be vacated
			bool movable = true;
			for (const auto& e : pendingMove.entries) {
				if (e.fromTrackIdx < 0 || e.fromTrackIdx >= (int)tracks.size() ||
					e.toTrackIdx < 0 || e.toTrackIdx >= (int)tracks.size())
					movable = false;
			}
			if (movable) {
				ClipEditScope scope(project, mContext.undoManager, "Move clip");
				for (const auto& e : pendingMove.entries) {
					scope.Touch(tracks[e.fromTrackIdx]);
					scope.Touch(tracks[e.toTrackIdx]);
				}
				{
					std::lock_guard<std::mutex> lock(project->GetMutex());
					for (const auto& e : pendingMove.entries)
						tracks[e.fromTrackIdx]->RemoveClip(e.clip);
					for (const auto& e : pendingMove.entries) {
						e.clip->SetStartBeat(e.newStartBeat);
						tracks[e.toTrackIdx]->AddClip(e.clip);
					}
				}
				// the track list follows the clip the gesture was anchored on
				int landedTrack = pendingMove.entries.back().toTrackIdx;
				for (const auto& e : pendingMove.entries) {
					if (e.clip == mContext.state.selectedClip)
						landedTrack = e.toTrackIdx;
				}
				mContext.state.SelectTrack(landedTrack);
				scope.Commit();
			}
		}
		if (pendingDelete.valid) {
			ClipEditScope scope(project, mContext.undoManager, "Delete clip");
			for (const auto& e : pendingDelete.entries) {
				if (e.trackIdx >= 0 && e.trackIdx < (int)tracks.size())
					scope.Touch(tracks[e.trackIdx]);
			}
			{
				std::lock_guard<std::mutex> lock(project->GetMutex());
				for (const auto& e : pendingDelete.entries) {
					if (e.trackIdx >= 0 && e.trackIdx < (int)tracks.size())
						tracks[e.trackIdx]->RemoveClip(e.clip);
				}
			}
			TimelineClipOps::PruneSelection(mContext);
			scope.Commit();
		}

		// preview os drag & drop
		if (mContext.state.isOsDragging) {
			bool insideTimeline = (mContext.state.osDragX >= pos.x && mContext.state.osDragX <= pos.x + timelineWidth &&
								   mContext.state.osDragY >= pos.y && mContext.state.osDragY <= pos.y + height);

			if (insideTimeline) {
				float relY = mContext.state.osDragY - trackAreaStartY;
				int trackIndex = (relY >= 0.0f && relY < TrackLayout::TotalHeight(rows)) ? TrackLayout::RowAtY(rows, relY) : -1;

				float relX = mContext.state.osDragX - winPos.x;
				double startBeat = (double)relX / mContext.state.pixelsPerBeat;
				if (startBeat < 0)
					startBeat = 0;
				if (mContext.state.timelineGrid > 0.0)
					startBeat = round(startBeat / mContext.state.timelineGrid) * mContext.state.timelineGrid;

				// no marker over a lane that cannot take the file (a group, or an open automation lane)
				if (trackIndex >= 0 && trackIndex < (int)tracks.size() && tracks[trackIndex]->AcceptsClips()) {
					// draw insertion marker (instead of fake box)
					float ghostX = winPos.x + (float)(startBeat * mContext.state.pixelsPerBeat);
					float ghostY = trackAreaStartY + rows[trackIndex].top;
					float ghostH = rows[trackIndex].height;

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
				// renaming one member of a selection names the whole block: a duplicated
				// run of clips gets relabelled in one pass instead of one dialog each
				if (mInteraction.clipToRename && mContext.state.IsClipSelected(mInteraction.clipToRename)) {
					for (const auto& sel : mContext.state.selectedClips)
						sel->SetName(mInteraction.renameBuffer);
				} else if (mInteraction.clipToRename) {
					mInteraction.clipToRename->SetName(mInteraction.renameBuffer);
				}
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

		ImGui::SetCursorScreenPos(ImVec2(winPos.x, trackAreaStartY + TrackLayout::TotalHeight(rows)));
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

			// the arrangement window's always-on vertical scrollbar occupies the far-right
			// lane, directly over this column. inset the track-list content by its width so
			// the volume/pan sliders, meters and buttons stay clear of it instead of tucking
			// underneath (hScrollbarSize == ScrollbarSize, same value for both scrollbars)
			float trackListContentW = trackListW - hScrollbarSize;

			drawList->AddRectFilled(
				ImVec2(trackListFixedPos.x, pos.y),
				ImVec2(trackListFixedPos.x + trackListContentW, pos.y + height - hScrollbarSize),
				th.bgHeader);
			drawList->AddLine(
				ImVec2(trackListFixedPos.x, pos.y),
				ImVec2(trackListFixedPos.x, pos.y + height - hScrollbarSize),
				th.divider, 2.0f);

			trackListView->Render(trackListFixedPos, trackListContentW, height, trackAreaStartY, stickyY, masterY);
		}
	}
	ImGui::End();
}
