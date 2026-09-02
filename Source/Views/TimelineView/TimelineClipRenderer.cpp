#include "PrecompHeader.h"
#include "TimelineClipRenderer.h"
#include "TimelineClipOps.h"
#include "TimelineUtils.h"
#include "TrackLayout.h"
#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include "Project.h"
#include "Theme.h"
#include <algorithm>
#include <cmath>
#include <mutex>

void TimelineClipRenderer::Render(EditorContext& context, TimelineInteractionState& interaction,
								  PendingClipMove& pendingMove, PendingClipDelete& pendingDelete,
								  Track* t, int trackIndex,
								  const ImVec2& winPos, float contentWidth, float viewWidth, float scrollX, float yPos, float rowHeight) {

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	Project* project = context.GetProject();
	ImGuiIO& io = ImGui::GetIO();

	// shared_ptr to this track, used when recording clip undo actions
	std::shared_ptr<Track> trackPtr = nullptr;
	if (project && trackIndex >= 0 && trackIndex < (int)project->GetTracks().size())
		trackPtr = project->GetTracks()[trackIndex];

	ImGui::SetCursorScreenPos(ImVec2(winPos.x + scrollX, yPos));
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton(("##TrackBG" + std::to_string(trackIndex)).c_str(), ImVec2(viewWidth, rowHeight));

	// a press on empty lane space starts a rubber band. only the anchor is recorded
	// here - the box spans tracks, so where it ends and what it caught is resolved
	// once per frame above the per-track loop, in TimelineTrackView
	if (ImGui::IsItemActivated()) {
		double anchorBeat = TimelineClipOps::SnapMarqueeBeat(context, (ImGui::GetMousePos().x - winPos.x) / context.state.pixelsPerBeat);

		interaction.clipMarqueeActive = true;
		interaction.clipMarqueeMoved = false;
		interaction.clipMarqueeStartBeat = anchorBeat;
		interaction.clipMarqueeEndBeat = anchorBeat;
		interaction.clipMarqueeStartTrack = trackIndex;
		interaction.clipMarqueeEndTrack = trackIndex;
		// a modifier means "add to what I already have"; a bare drag starts fresh
		interaction.clipMarqueeBase.clear();
		if (io.KeyCtrl || io.KeyShift)
			interaction.clipMarqueeBase = context.state.selectedClips;

		context.state.SelectTrack(trackIndex);
	}

	if (ImGui::BeginPopupContextItem()) {
		double clickBeat = (ImGui::GetMousePos().x - winPos.x) / context.state.pixelsPerBeat;
		if (context.state.timelineGrid > 0.0)
			clickBeat = round(clickBeat / context.state.timelineGrid) * context.state.timelineGrid;
		if (clickBeat < 0)
			clickBeat = 0;

		// MenuItem rather than Selectable throughout the clip menus: it is the only
		// entry that carries a shortcut column, and the arrangement's keys are the fast
		// path that the menu is supposed to teach
		if (ImGui::MenuItem("Add MIDI Clip")) {
			auto clip = std::make_shared<MIDIClip>();
			clip->SetName("New Clip");
			clip->SetStartBeat(clickBeat);
			clip->SetDuration(4.0);
			auto before = ClipSnapshotAction::Snapshot(trackPtr);
			t->AddClip(clip);
			if (trackPtr)
				context.undoManager.Push(std::make_unique<ClipSnapshotAction>(project, trackPtr, before, ClipSnapshotAction::Snapshot(trackPtr), "Add clip"));
			context.state.SelectClip(clip);
		}
		if (!interaction.clipboard.empty()) {
			ImGui::Separator();
			bool multi = interaction.clipboard.size() > 1;
			if (ImGui::MenuItem(multi ? "Paste Clips" : "Paste", "Ctrl+V")) {
				// the block lands with its top-left corner where the menu was opened
				TimelineClipOps::PasteAt(context, interaction, clickBeat, trackIndex);
				context.state.SelectTrack(trackIndex);
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
		// only what the interaction below needs to gate itself is sampled up here.
		// everything the clip is DRAWN with is read after that interaction has run,
		// down in the visuals block - see the note there
		bool isFocused = (context.state.selectedClip == clip);

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
				// a right-click outside the current selection retargets it, so the menu
				// always describes the clips it is about to act on
				if (!context.state.IsClipSelected(clip)) {
					context.state.SelectClip(clip);
					context.state.SelectTrack(trackIndex);
				}
				int selectedCount = (int)context.state.selectedClips.size();
				if (selectedCount > 1)
					ImGui::TextDisabled("%d clips selected", selectedCount);

				if (ImGui::MenuItem("Copy", "Ctrl+C"))
					TimelineClipOps::CopySelection(context, interaction);
				if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
					TimelineClipOps::DuplicateSelection(context);
				if (ImGui::MenuItem("Split At Marker", "Ctrl+E"))
					TimelineClipOps::SplitSelectionAt(context, context.state.selectionStart);
				if (ImGui::MenuItem("Rename", "F2")) {
					interaction.clipToRename = clip;
					strncpy(interaction.renameBuffer, clip->GetName().c_str(), sizeof(interaction.renameBuffer));
					interaction.renameBuffer[sizeof(interaction.renameBuffer) - 1] = 0;
					interaction.triggerRenamePopup = true;
					ImGui::CloseCurrentPopup();
				}
				if (ImGui::MenuItem(clip->IsEnabled() ? "Deactivate" : "Activate", "D"))
					TimelineClipOps::ToggleSelectionEnabled(context);

				// only offered when something in the selection is audio: there is nothing
				// a note list plays backwards
				if (TimelineClipOps::SelectionHasAudio(context) && ImGui::MenuItem("Reverse"))
					TimelineClipOps::ReverseSelection(context);

				// only worth offering when it would actually detach something: a clip
				// nothing else shares notes with is already unique
				bool anyLinked = false;
				for (const auto& sel : context.state.selectedClips) {
					if (auto selMIDI = std::dynamic_pointer_cast<MIDIClip>(sel))
						anyLinked = anyLinked || selMIDI->IsSequenceShared();
				}
				if (anyLinked && ImGui::MenuItem("Make Unique")) {
					// swapping the note vector out from under the sequencer, which walks
					// it on the audio thread for the whole block
					std::unique_lock<std::mutex> lock;
					if (project)
						lock = std::unique_lock<std::mutex>(project->GetMutex());
					for (const auto& sel : context.state.selectedClips) {
						if (auto selMIDI = std::dynamic_pointer_cast<MIDIClip>(sel))
							selMIDI->MakeUnique();
					}
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Delete", "Del")) {
					// deferred: removing clips here would erase from the vector this
					// loop copied its list from while the popup is still up
					for (const auto& r : TimelineClipOps::ResolveSelection(context))
						pendingDelete.entries.push_back({r.clip, r.trackIndex});
					pendingDelete.valid = !pendingDelete.entries.empty();
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
				interaction.dragCollapseCandidate = nullptr;
				interaction.dragMoved = false;

				if (io.KeyCtrl) {
					context.state.ToggleClipSelection(clip);
				} else if (io.KeyShift) {
					TimelineClipOps::SelectRangeTo(context, clip);
				} else if (context.state.IsClipSelected(clip)) {
					// keep the block intact so the drag can carry all of it. a press
					// that turns out not to be a drag collapses to this clip on release
					context.state.selectedClip = clip;
					interaction.dragCollapseCandidate = clip;
				} else {
					context.state.SelectClip(clip);
				}
				context.state.SelectTrack(trackIndex);

				// a Ctrl+click that took the clip back out of the selection is a
				// deselect, not the start of a drag
				if (context.state.IsClipSelected(clip)) {
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

					// every selected clip travels with the gesture; capture the geometry
					// each of them started from so the deltas stay relative
					interaction.dragEntries.clear();
					for (const auto& r : TimelineClipOps::ResolveSelection(context)) {
						interaction.dragEntries.push_back({r.clip, r.trackIndex,
														   r.clip->GetStartBeat(), r.clip->GetDuration(), r.clip->GetOffset()});
					}

					if (nearLeft)
						interaction.dragState = DragState::ResizingLeft;
					else if (nearRight)
						interaction.dragState = DragState::ResizingRight;
					else
						interaction.dragState = DragState::Moving;
				}
			}

			if (isActive && isFocused && interaction.dragState != DragState::None && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
				// past the drag threshold this stops being a click and becomes a gesture:
				// only now do the ghosts appear and the click-to-collapse get called off
				if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
					interaction.dragMoved = true;
				ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
				double deltaBeats = dragDelta.x / context.state.pixelsPerBeat;

				if (interaction.dragState == DragState::Moving) {
					// 1. calculate new beat
					double newStart = interaction.dragOriginalStart + deltaBeats;
					if (context.state.timelineGrid > 0.0)
						newStart = round(newStart / context.state.timelineGrid) * context.state.timelineGrid;

					// the whole block stops at the left edge together: the earliest clip
					// in the selection decides how far the gesture can travel, otherwise
					// clamping the anchor alone would squash the block against beat 0
					double delta = newStart - interaction.dragOriginalStart;
					double earliest = interaction.dragOriginalStart;
					for (const auto& e : interaction.dragEntries)
						earliest = std::min(earliest, e.startBeat);
					if (earliest + delta < 0.0)
						delta = -earliest;

					interaction.dragCurrentBeat = interaction.dragOriginalStart + delta;

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
							// with several clips in flight the whole block has to fit, so one member
							// landing on a group lane holds all of them at the last legal row
							auto& projectTracks = project->GetTracks();
							int trackDelta = targetIdx - trackIndex;
							bool blockFits = true;
							for (const auto& e : interaction.dragEntries) {
								int landing = e.trackIdx + trackDelta;
								if (landing < 0 || landing >= (int)projectTracks.size() ||
									!projectTracks[landing]->AcceptsClips() ||
									landing >= (int)rows.size() || !rows[landing].visible) {
									blockFits = false;
									break;
								}
							}
							if (blockFits)
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
			if (interaction.dragState != DragState::None && isFocused && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				TimelineClipOps::CommitDrag(context, interaction, pendingMove);

				// the press landed on a clip that was already part of a multi-selection
				// and never turned into a drag: treat it as the plain click it was and
				// narrow the selection down to it
				if (interaction.dragCollapseCandidate && !interaction.dragMoved)
					context.state.SelectClip(interaction.dragCollapseCandidate);

				// reset
				interaction.dragState = DragState::None;
				interaction.dragSourceTrackIdx = -1;
				interaction.dragTargetTrackIdx = -1;
				interaction.dragEntries.clear();
				interaction.dragCollapseCandidate = nullptr;
				interaction.dragMoved = false;
			}

			ImGui::PopID();

			// visuals
			// NOTE: every flag the clip is drawn with is read HERE, after the interaction
			// above, never before it. a click that changed the selection has already been
			// applied by this point, so the clip lights up on the frame the mouse went
			// down instead of the next one - sampling these at the top of the loop is what
			// made a freshly clicked clip blink
			bool drawSelected = context.state.IsClipSelected(clip);
			bool drawFocused = (context.state.selectedClip == clip);
			// the gesture is anchored on the focused clip, but every selected clip rides
			// along with it, so all of them dim and show a ghost - once it has actually
			// travelled. a press that never moves is a click, and must not flash
			bool drawDragging = (interaction.dragState != DragState::None && interaction.dragMoved && drawSelected);

			// a deactivated clip drops the track color for a neutral grey, so a glance at the
			// arrangement says which clips are going to sound
			ImU32 baseColor = clip->IsEnabled() ? t->GetColor() : Theme::Instance().clipDisabled;

			// if dragging, the original clip stays in place but dimmed
			if (drawDragging) {
				baseColor = Theme::WithAlpha(Theme::Instance().textDim, 60); // ghostly
			} else if (!drawSelected) {
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

			// selection outline, drawn over the content. every member of the selection
			// gets the bright stroke; the focused one - the clip the piano roll and the
			// clip view are showing, and the clip a drag measures its deltas from - also
			// gets the accent, so a block of twenty still says which one is being edited
			if (drawSelected && !drawDragging) {
				const Theme& th = Theme::Instance();
				drawList->AddRect(pMin, pMax, th.selectionStroke, 0.0f, 0, 2.0f);
				if (drawFocused && context.state.selectedClips.size() > 1)
					drawList->AddRect(ImVec2(pMin.x + 2, pMin.y + 2), ImVec2(pMax.x - 2, pMax.y - 2), th.accent, 0.0f, 0, 2.0f);
			}
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

	auto audioClip = std::dynamic_pointer_cast<AudioClip>(clip);
	auto mIDIClip = std::dynamic_pointer_cast<MIDIClip>(clip);

	// a clip whose notes another clip also plays gets its own border color and a chain
	// badge, because an edit made in the piano roll silently lands on every one of them
	const bool linked = mIDIClip && mIDIClip->IsSequenceShared();

	drawList->AddRectFilled(pMin, pMax, baseColor, 0.0f);
	drawList->AddRect(pMin, pMax, linked ? th.clipLinked : th.clipBorder, 0.0f);
	drawList->PushClipRect(pMin, pMax, true);

	// safe culling rect
	ImVec2 clipRectMin = drawList->GetClipRectMin();
	ImVec2 clipRectMax = drawList->GetClipRectMax();

	double effectiveOffset = (overrideOffset >= 0.0) ? overrideOffset : clip->GetOffset();
	double effectiveDuration = (overrideDuration >= 0.0) ? overrideDuration : clip->GetDuration();

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

	// the chain badge rides the top-right corner, out of the way of the name on the
	// left and of the "MIDI" tag at the bottom
	float badgeWidth = 0.0f;
	if (linked) {
		const float badgeHeight = std::min(ImGui::GetTextLineHeight() * 0.72f, (pMax.y - pMin.y) - 6.0f);
		if (badgeHeight > 3.0f) {
			badgeWidth = TimelineUtils::LinkBadgeWidth(badgeHeight);
			TimelineUtils::DrawLinkBadge(drawList, ImVec2(pMax.x - badgeWidth - 3.0f, pMin.y + 3.0f), badgeHeight,
										 enabled ? th.clipLinked : Theme::WithAlpha(th.clipLinked, 110));
		}
	}

	// calculate text position
	const char* clipName = clip->GetName().c_str();
	ImVec2 textSize = ImGui::CalcTextSize(clipName);
	float textPadding = 4.0f;

	// start at the clip's visible left edge
	float textX = clipRectMin.x + textPadding;

	// prevent text from sliding off the right side, and out from under the badge
	float maxTextX = pMax.x - textSize.x - textPadding - (badgeWidth > 0.0f ? badgeWidth + 3.0f : 0.0f);
	if (textX > maxTextX)
		textX = maxTextX;

	// ensure we don't draw before the actual clip start (handles tiny clips)
	if (textX < pMin.x + textPadding)
		textX = pMin.x + textPadding;

	drawList->AddText(ImVec2(textX, pMin.y + textPadding), enabled ? th.clipText : th.clipTextDim, clipName);

	drawList->PopClipRect();
}
