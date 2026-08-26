#include "PrecompHeader.h"
#include "TimelineClipRenderer.h"
#include "TimelineUtils.h"
#include "TrackLayout.h"
#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include "Project.h"
#include "Theme.h"
#include <algorithm>
#include <cmath>

void TimelineClipRenderer::Render(EditorContext& context, TimelineInteractionState& interaction,
								  PendingClipMove& pendingMove, PendingClipDelete& pendingDelete,
								  Track* t, int trackIndex,
								  const ImVec2& winPos, float contentWidth, float viewWidth, float scrollX, float yPos, float rowHeight) {

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	Project* project = context.GetProject();

	// shared_ptr to this track, used when recording clip undo actions
	std::shared_ptr<Track> trackPtr = nullptr;
	if (project && trackIndex >= 0 && trackIndex < (int)project->GetTracks().size())
		trackPtr = project->GetTracks()[trackIndex];

	ImGui::SetCursorScreenPos(ImVec2(winPos.x + scrollX, yPos));
	ImGui::SetNextItemAllowOverlap();
	if (ImGui::InvisibleButton(("##TrackBG" + std::to_string(trackIndex)).c_str(), ImVec2(viewWidth, rowHeight))) {
		context.state.selectedClip = nullptr;
		context.state.SelectTrack(trackIndex);
	}
	if (ImGui::BeginPopupContextItem()) {
		double clickBeat = (ImGui::GetMousePos().x - winPos.x) / context.state.pixelsPerBeat;
		if (context.state.timelineGrid > 0.0)
			clickBeat = round(clickBeat / context.state.timelineGrid) * context.state.timelineGrid;
		if (clickBeat < 0)
			clickBeat = 0;

		if (ImGui::Selectable("Add MIDI Clip")) {
			auto clip = std::make_shared<MIDIClip>();
			clip->SetName("New Clip");
			clip->SetStartBeat(clickBeat);
			clip->SetDuration(4.0);
			auto before = ClipSnapshotAction::Snapshot(trackPtr);
			t->AddClip(clip);
			if (trackPtr)
				context.undoManager.Push(std::make_unique<ClipSnapshotAction>(project, trackPtr, before, ClipSnapshotAction::Snapshot(trackPtr), "Add clip"));
		}
		if (interaction.clipboard) {
			ImGui::Separator();
			if (ImGui::Selectable("Paste")) {
				auto newClip = CloneClip(interaction.clipboard);
				if (newClip) {
					newClip->SetStartBeat(clickBeat);
					auto before = ClipSnapshotAction::Snapshot(trackPtr);
					t->AddClip(newClip);
					if (trackPtr)
						context.undoManager.Push(std::make_unique<ClipSnapshotAction>(project, trackPtr, before, ClipSnapshotAction::Snapshot(trackPtr), "Paste clip"));
					context.state.selectedClip = newClip;
					context.state.SelectTrack(trackIndex);
				}
			}
		}
		ImGui::EndPopup();
	}

	// draw clips
	// iterate a copy of the list, not the live one: committing a drag calls ResolveOverlaps
	// and the "Duplicate" menu entry calls AddClip, both of which erase from / push onto the
	// track's clip vector from inside this loop and would invalidate its iterators
	std::vector<std::shared_ptr<Clip>> clips = t->GetClips();
	for (auto& clip : clips) {
		bool isDraggingThis = (interaction.dragState != DragState::None && context.state.selectedClip == clip);

		// we always draw the "original" state here. if dragging, it becomes the "background/placeholder"
		double drawStart = clip->GetStartBeat();
		double drawDur = clip->GetDuration();

		float clipStartX = winPos.x + (float)(drawStart * context.state.pixelsPerBeat);
		float clipWidth = (float)(drawDur * context.state.pixelsPerBeat);
		// a clip must never come out narrower than a pixel: zoomed far enough out it rounds
		// to nothing, and a project saved before clip lengths were re-read on a tempo change
		// can hold one that is genuinely zero beats long. either way ImGui asserts on a
		// zero-size item, and a clip too small to click is one the user cannot delete
		clipWidth = std::max(clipWidth, 1.0f);
		float clipEndX = clipStartX + clipWidth;

		if (clipEndX > winPos.x && clipStartX < winPos.x + viewWidth + scrollX) {
			ImVec2 pMin(clipStartX, yPos + 1);
			ImVec2 pMax(clipEndX, yPos + rowHeight - 1);

			ImGui::SetCursorScreenPos(pMin);
			ImGui::PushID(clip.get());

			ImGui::SetNextItemAllowOverlap();
			ImGui::InvisibleButton("##ClipHit", ImVec2(clipWidth, rowHeight - 2));

			if (ImGui::BeginPopupContextItem()) {
				if (ImGui::Selectable("Copy")) {
					interaction.clipboard = CloneClip(clip);
					context.state.selectedClip = clip;
				}
				if (ImGui::Selectable("Duplicate")) {
					auto newClip = CloneClip(clip);
					if (newClip) {
						newClip->SetStartBeat(clip->GetEndBeat());
						auto before = ClipSnapshotAction::Snapshot(trackPtr);
						t->AddClip(newClip);
						if (trackPtr)
							context.undoManager.Push(std::make_unique<ClipSnapshotAction>(project, trackPtr, before, ClipSnapshotAction::Snapshot(trackPtr), "Duplicate clip"));
						context.state.selectedClip = newClip;
					}
				}
				if (ImGui::Selectable("Rename")) {
					interaction.clipToRename = clip;
					strncpy(interaction.renameBuffer, clip->GetName().c_str(), sizeof(interaction.renameBuffer));
					interaction.renameBuffer[sizeof(interaction.renameBuffer) - 1] = '\0';
					interaction.triggerRenamePopup = true;
					ImGui::CloseCurrentPopup();
				}
				if (ImGui::Selectable(clip->IsEnabled() ? "Deactivate" : "Activate")) {
					ToggleClipEnabled(project, context.undoManager, trackPtr, clip);
				}
				auto mIDIClip = std::dynamic_pointer_cast<MIDIClip>(clip);
				if (mIDIClip) {
					if (ImGui::Selectable("Make Unique")) {
						mIDIClip->MakeUnique();
					}
				}
				ImGui::Separator();
				if (ImGui::Selectable("Delete")) {
					pendingDelete.clip = clip;
					pendingDelete.trackIdx = trackIndex;
					pendingDelete.valid = true;
				}
				ImGui::EndPopup();
			}

			// mouse interaction logic
			bool isHovered = ImGui::IsItemHovered();
			bool isActivated = ImGui::IsItemActivated();
			bool isActive = ImGui::IsItemActive();
			float mouseRelX = ImGui::GetMousePos().x - pMin.x;
			bool nearLeft = mouseRelX < 8.0f;
			bool nearRight = mouseRelX > clipWidth - 8.0f;

			if (isHovered)
				ImGui::SetMouseCursor((nearLeft || nearRight) ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_Hand);

			if (isActivated) {
				context.state.selectedClip = clip;
				context.state.SelectTrack(trackIndex);

				// snapshot the track's clips for undo of the drag/resize about to begin
				interaction.dragClipsBefore = ClipSnapshotAction::Snapshot(trackPtr);

				interaction.dragOriginalStart = clip->GetStartBeat();
				interaction.dragOriginalDuration = clip->GetDuration();
				interaction.dragOriginalOffset = clip->GetOffset();

				// initialize drag state
				interaction.dragSourceTrackIdx = trackIndex;
				interaction.dragTargetTrackIdx = trackIndex;

				// init dynamic values
				interaction.dragCurrentBeat = clip->GetStartBeat();
				interaction.dragCurrentDuration = clip->GetDuration();
				interaction.dragCurrentOffset = clip->GetOffset();

				if (nearLeft)
					interaction.dragState = DragState::ResizingLeft;
				else if (nearRight)
					interaction.dragState = DragState::ResizingRight;
				else
					interaction.dragState = DragState::Moving;
			}

			if (isActive && isDraggingThis && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
				ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
				double deltaBeats = dragDelta.x / context.state.pixelsPerBeat;

				if (interaction.dragState == DragState::Moving) {
					// 1. calculate new beat
					double newStart = interaction.dragOriginalStart + deltaBeats;
					if (context.state.timelineGrid > 0.0)
						newStart = round(newStart / context.state.timelineGrid) * context.state.timelineGrid;
					if (newStart < 0)
						newStart = 0;

					interaction.dragCurrentBeat = newStart;

					// 2. calculate target track (accounts for variable row heights /
					// collapsed lanes via the shared layout)
					if (project) {
						auto rows = TrackLayout::Build(context);
						if (trackIndex >= 0 && trackIndex < (int)rows.size()) {
							float trackAreaTop = yPos - rows[trackIndex].top;
							float relY = ImGui::GetMousePos().y - trackAreaTop;
							int targetIdx = TrackLayout::RowAtY(rows, relY);
							if (targetIdx < 0)
								targetIdx = trackIndex;
							// hovering a lane that cannot take clips (a group, or a track with its
							// automation lane open) must not retarget the drag: the drop would be
							// refused on release and the clip would snap back with no explanation.
							// leave the ghost on the last lane that can actually take it
							auto& projectTracks = project->GetTracks();
							if (targetIdx >= 0 && targetIdx < (int)projectTracks.size() && projectTracks[targetIdx]->AcceptsClips())
								interaction.dragTargetTrackIdx = targetIdx;
						}
					}

				} else if (interaction.dragState == DragState::ResizingRight) {
					// update interaction state
					double newDur = interaction.dragOriginalDuration + deltaBeats;
					if (context.state.timelineGrid > 0.0)
						newDur = round(newDur / context.state.timelineGrid) * context.state.timelineGrid;
					if (newDur < context.state.timelineGrid)
						newDur = context.state.timelineGrid;

					// check audio limits
					auto audioClip = std::dynamic_pointer_cast<AudioClip>(clip);
					if (audioClip) {
						double projectBpm = project ? project->GetTransport().GetBpm() : 120.0;
						double maxDur = audioClip->GetMaxDurationInBeats(projectBpm);
						double maxAllowed = maxDur - interaction.dragOriginalOffset;
						if (maxAllowed < context.state.timelineGrid)
							maxAllowed = context.state.timelineGrid;
						if (newDur > maxAllowed)
							newDur = maxAllowed;
					}

					interaction.dragCurrentDuration = newDur;
					// offset and start remain original for right resize

				} else if (interaction.dragState == DragState::ResizingLeft) {
					double newStart = interaction.dragOriginalStart + deltaBeats;
					if (context.state.timelineGrid > 0.0)
						newStart = round(newStart / context.state.timelineGrid) * context.state.timelineGrid;
					if (newStart < 0)
						newStart = 0;

					double endTime = interaction.dragOriginalStart + interaction.dragOriginalDuration;
					double actualDelta = newStart - interaction.dragOriginalStart;
					double newOffset = interaction.dragOriginalOffset + actualDelta;

					// clamping if offset goes negative
					if (newOffset < 0) {
						newOffset = 0;
						newStart = interaction.dragOriginalStart - interaction.dragOriginalOffset;
						if (context.state.timelineGrid > 0.0)
							newStart = round(newStart / context.state.timelineGrid) * context.state.timelineGrid;
					}

					if (newStart < endTime - context.state.timelineGrid) {
						interaction.dragCurrentBeat = newStart;
						interaction.dragCurrentDuration = endTime - newStart;
						interaction.dragCurrentOffset = newOffset;
					}
				}
			}

			// detect mouse release to commit changes
			if (interaction.dragState != DragState::None && isDraggingThis && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				if (interaction.dragState == DragState::Moving) {
					if (interaction.dragTargetTrackIdx != interaction.dragSourceTrackIdx) {
						// cross track
						auto& tracks = project->GetTracks();
						if (interaction.dragTargetTrackIdx >= 0 && interaction.dragTargetTrackIdx < (int)tracks.size()) {
							if (tracks[interaction.dragTargetTrackIdx]->AcceptsClips()) {
								pendingMove.clip = clip;
								pendingMove.fromTrackIdx = trackIndex;
								pendingMove.toTrackIdx = interaction.dragTargetTrackIdx;
								pendingMove.newStartBeat = interaction.dragCurrentBeat;
								pendingMove.valid = true;
							}
						}
					} else {
						// same track
						bool moved = (interaction.dragCurrentBeat != interaction.dragOriginalStart);
						clip->SetStartBeat(interaction.dragCurrentBeat);
						t->ResolveOverlaps(clip);
						if (trackPtr && moved)
							context.undoManager.Push(std::make_unique<ClipSnapshotAction>(project, trackPtr, interaction.dragClipsBefore, ClipSnapshotAction::Snapshot(trackPtr), "Move clip"));
					}
				} else if (interaction.dragState == DragState::ResizingLeft) {
					bool changed = (interaction.dragCurrentBeat != interaction.dragOriginalStart) ||
								   (interaction.dragCurrentDuration != interaction.dragOriginalDuration) ||
								   (interaction.dragCurrentOffset != interaction.dragOriginalOffset);
					clip->SetStartBeat(interaction.dragCurrentBeat);
					clip->SetDuration(interaction.dragCurrentDuration);
					clip->SetOffset(interaction.dragCurrentOffset);
					t->ResolveOverlaps(clip);
					if (trackPtr && changed)
						context.undoManager.Push(std::make_unique<ClipSnapshotAction>(project, trackPtr, interaction.dragClipsBefore, ClipSnapshotAction::Snapshot(trackPtr), "Resize clip"));
				} else if (interaction.dragState == DragState::ResizingRight) {
					bool changed = (interaction.dragCurrentDuration != interaction.dragOriginalDuration);
					clip->SetDuration(interaction.dragCurrentDuration);
					t->ResolveOverlaps(clip);
					if (trackPtr && changed)
						context.undoManager.Push(std::make_unique<ClipSnapshotAction>(project, trackPtr, interaction.dragClipsBefore, ClipSnapshotAction::Snapshot(trackPtr), "Resize clip"));
				}

				// reset
				interaction.dragState = DragState::None;
				interaction.dragSourceTrackIdx = -1;
				interaction.dragTargetTrackIdx = -1;
			}

			ImGui::PopID();

			// visuals
			bool isSelected = (context.state.selectedClip == clip);
			// a deactivated clip drops the track color for a neutral grey, so a glance at the
			// arrangement says which clips are going to sound
			ImU32 baseColor = clip->IsEnabled() ? t->GetColor() : Theme::Instance().clipDisabled;

			// if dragging, the original clip stays in place but dimmed
			if (isDraggingThis) {
				baseColor = Theme::WithAlpha(Theme::Instance().textDim, 60); // ghostly
			} else if (!isSelected) {
				ImVec4 c = ImGui::ColorConvertU32ToFloat4(baseColor);
				c.w = 0.8f;
				baseColor = ImGui::ColorConvertFloat4ToU32(c);
			} else {
				ImVec4 c = ImGui::ColorConvertU32ToFloat4(baseColor);
				c.x = std::min(1.0f, c.x * 1.2f);
				c.y = std::min(1.0f, c.y * 1.2f);
				c.z = std::min(1.0f, c.z * 1.2f);
				baseColor = ImGui::ColorConvertFloat4ToU32(c);
			}

			// render using the clip's actual current data
			DrawClipContent(drawList, clip, pMin, pMax, winPos, viewWidth, context, baseColor);
		}
	}
}

void TimelineClipRenderer::DrawClipContent(ImDrawList* drawList,
										   std::shared_ptr<Clip> clip,
										   const ImVec2& pMin, const ImVec2& pMax,
										   const ImVec2& winPos, float viewWidth,
										   EditorContext& context,
										   ImU32 baseColor,
										   double overrideStartBeat,
										   double overrideDuration,
										   double overrideOffset,
										   ImU32 customWaveColor,
										   ImU32 customMIDIColor) {

	Project* project = context.GetProject();
	const Theme& th = Theme::Instance();
	// the content of a deactivated clip recedes with its body: still readable, but
	// clearly not part of what is playing. a ghost preview passes its own colors
	bool enabled = clip->IsEnabled();

	drawList->AddRectFilled(pMin, pMax, baseColor, 0.0f);
	drawList->AddRect(pMin, pMax, th.clipBorder, 0.0f);
	drawList->PushClipRect(pMin, pMax, true);

	// safe culling rect
	ImVec2 clipRectMin = drawList->GetClipRectMin();
	ImVec2 clipRectMax = drawList->GetClipRectMax();

	double effectiveOffset = (overrideOffset >= 0.0) ? overrideOffset : clip->GetOffset();
	double effectiveDuration = (overrideDuration >= 0.0) ? overrideDuration : clip->GetDuration();

	auto audioClip = std::dynamic_pointer_cast<AudioClip>(clip);
	auto mIDIClip = std::dynamic_pointer_cast<MIDIClip>(clip);

	if (audioClip) {
		const auto& samples = audioClip->GetSamples();
		if (!samples.empty()) {
			int channels = audioClip->GetNumChannels();

			ImU32 waveColor = customWaveColor != 0
								  ? customWaveColor
								  : ((channels == 2) ? th.waveBgMono : th.waveBgMid);
			if (!enabled && customWaveColor == 0)
				waveColor = Theme::WithAlpha(waveColor, 70);

			double playbackRate = 1.0;
			double projectSR = 48000.0;
			double projectBpm = 120.0;

			if (project) {
				projectSR = project->GetTransport().GetSampleRate();
				projectBpm = project->GetTransport().GetBpm();
				if (projectSR == 0.0)
					projectSR = 48000.0;
				// map pixels to source frames the same way the audio thread advances through the
				// file. granular modes advance at the pitch-independent time base, so the waveform
				// keeps its width regardless of transpose; Re-Pitch/unwarped use the full varispeed
				// rate where pitch does compress the drawn waveform
				playbackRate = audioClip->UsesGranularEngine()
								   ? audioClip->ComputeTimeStretchRate(projectSR, projectBpm)
								   : audioClip->ComputePlaybackRate(projectSR, projectBpm);
			}

			// calculate density
			double timelineSecondsPerPixel = (60.0 / projectBpm) / context.state.pixelsPerBeat;
			double outputFramesPerPixel = timelineSecondsPerPixel * projectSR;
			double sourceFramesPerPixel = outputFramesPerPixel * playbackRate;

			// use effective offset for rendering the correct part of the waveform
			double offsetSeconds = effectiveOffset * (60.0 / projectBpm);
			double offsetOutputFrames = offsetSeconds * projectSR;
			double offsetSourceFrames = offsetOutputFrames * playbackRate;

			TimelineUtils::RenderWaveform(drawList, samples, channels, sourceFramesPerPixel, offsetSourceFrames, pMin, pMax, waveColor);
		}
	}

	if (mIDIClip) {
		ImGui::SetCursorScreenPos(ImVec2(pMin.x + 2, pMax.y - 12));
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::WithAlpha(th.textOnAccent, 128)), "MIDI");

		const auto& notes = mIDIClip->GetNotesEx();
		float clipWidth = pMax.x - pMin.x;

		ImU32 noteColor = customMIDIColor != 0
							  ? customMIDIColor
							  : Theme::WithAlpha(th.clipText, 160);
		if (!enabled && customMIDIColor == 0)
			noteColor = Theme::WithAlpha(noteColor, 70);

		for (const auto& n : notes) {
			// use effective offset for MIDI note culling/positioning
			double relStart = n.startBeat - effectiveOffset;
			if (relStart + n.durationBeats < 0)
				continue;

			// note position is percentage of the effective duration
			float nx = pMin.x + (float)(relStart / effectiveDuration) * clipWidth;
			float nw = (float)(n.durationBeats / effectiveDuration) * clipWidth;
			if (nw < 2.0f)
				nw = 2.0f;

			// cull
			if (nx + nw < clipRectMin.x || nx > clipRectMax.x)
				continue;

			if (nx < pMin.x) {
				nw -= (pMin.x - nx);
				nx = pMin.x;
			}

			float tN = (float)n.noteNumber / 127.0f;
			float center_y = (pMax.y - 5.0f) * (1.0f - tN) + (pMin.y + 5.0f) * tN;
			drawList->AddRectFilled(ImVec2(nx, center_y - 1), ImVec2(nx + nw, center_y + 1), noteColor);
		}
	}

	// calculate text position
	const char* clipName = clip->GetName().c_str();
	ImVec2 textSize = ImGui::CalcTextSize(clipName);
	float textPadding = 4.0f;

	// start at the clip's visible left edge
	float textX = clipRectMin.x + textPadding;

	// prevent text from sliding off the right side
	float maxTextX = pMax.x - textSize.x - textPadding;
	if (textX > maxTextX)
		textX = maxTextX;

	// ensure we don't draw before the actual clip start (handles tiny clips)
	if (textX < pMin.x + textPadding)
		textX = pMin.x + textPadding;

	drawList->AddText(ImVec2(textX, pMin.y + textPadding), enabled ? th.clipText : th.clipTextDim, clipName);

	drawList->PopClipRect();
}
