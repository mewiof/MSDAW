#include "PrecompHeader.h"
#include "TimelineAutomationRenderer.h"
#include "TimelineClipRenderer.h"
#include "TimelineUtils.h"
#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include "Project.h"
#include "AutomationEdits.h"
#include "Undo/Actions.h"
#include "Theme.h"
#include <algorithm>
#include <cmath>

void TimelineAutomationRenderer::Render(EditorContext& context, TimelineInteractionState& interaction,
										Track* t, int trackIndex,
										const ImVec2& winPos, float contentWidth, float viewWidth, float scrollX, float yPos) {

	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImGuiIO& io = ImGui::GetIO();

	Project* project = context.GetProject();

	// resolve the owning shared_ptr (handles both regular tracks and master) so
	// automation undo actions keep the track alive
	std::shared_ptr<Track> trackPtr = nullptr;
	if (project) {
		for (auto& tr : project->GetTracks()) {
			if (tr.get() == t) {
				trackPtr = tr;
				break;
			}
		}
		if (!trackPtr && project->GetMasterTrack().get() == t)
			trackPtr = project->GetMasterTrack();
	}

	// compare curves ignoring the transient `selected` flag
	auto pointsEqual = [](const std::vector<AutomationPoint>& a, const std::vector<AutomationPoint>& b) {
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i) {
			if (a[i].beat != b[i].beat || a[i].value != b[i].value || a[i].tension != b[i].tension)
				return false;
		}
		return true;
	};

	ImVec2 trackMin(winPos.x, yPos);
	ImVec2 trackMax(winPos.x + contentWidth, yPos + context.layout.trackRowHeight);

	// draw faded clips (reference background)
	auto& clips = t->GetClips();
	for (auto& clip : clips) {
		float clipStartX = winPos.x + (float)(clip->GetStartBeat() * context.state.pixelsPerBeat);
		float clipWidth = (float)(clip->GetDuration() * context.state.pixelsPerBeat);
		float clipEndX = clipStartX + clipWidth;

		if (clipEndX > winPos.x && clipStartX < winPos.x + viewWidth + scrollX) {
			ImVec2 pMin(clipStartX, yPos + 1);
			ImVec2 pMax(clipEndX, yPos + context.layout.trackRowHeight - 1);

			// automation background view
			ImU32 ghostBgColor = Theme::WithAlpha(th.bgPanelAlt, 100);
			ImU32 ghostWaveColor = Theme::WithAlpha(th.waveBgMid, 60);
			ImU32 ghostMIDIColor = th.automationGhost;

			TimelineClipRenderer::DrawClipContent(drawList, clip, pMin, pMax, winPos, viewWidth,
												  context, ghostBgColor, -1.0, -1.0, -1.0,
												  ghostWaveColor, ghostMIDIColor);
		}
	}

	// full-width invisible button for background interaction
	ImGui::SetCursorScreenPos(ImVec2(winPos.x + scrollX, trackMin.y));
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("##AutomationBlocker", ImVec2(viewWidth, context.layout.trackRowHeight));

	bool isTrackHovered = ImGui::IsItemHovered();
	bool isTrackActive = ImGui::IsItemActive();
	bool isTrackClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
	bool isTrackRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

	// automation dropdown
	std::vector<Parameter*> allParams = t->GetAllParameters();
	if (!t->mSelectedAutomationParam && !allParams.empty())
		t->mSelectedAutomationParam = allParams[0];

	ImGui::SetCursorScreenPos(ImVec2(winPos.x + 5 + scrollX, yPos + 2));
	ImGui::PushItemWidth(120);
	std::string comboLabel = t->mSelectedAutomationParam ? t->mSelectedAutomationParam->name : "None";
	if (ImGui::BeginCombo(("##AutoParam" + std::to_string(trackIndex)).c_str(), comboLabel.c_str())) {
		for (auto param : allParams) {
			bool isSelected = (t->mSelectedAutomationParam == param);
			if (ImGui::Selectable(param->name.c_str(), isSelected)) {
				t->mSelectedAutomationParam = param;
			}
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::PopItemWidth();

	// draw & interact with automation curve
	if (t->mSelectedAutomationParam) {
		AutomationCurve* curve = t->GetAutomationCurve(t->mSelectedAutomationParam);
		float minVal = t->mSelectedAutomationParam->minValue;
		float maxVal = t->mSelectedAutomationParam->maxValue;
		float range = maxVal - minVal;
		if (range <= 0.0001f)
			range = 1.0f; // prevent divide by zero

		float padY = 12.0f; // vertical padding to keep points grabbable away from edges
		float curveTopY = trackMin.y + padY;
		float curveBottomY = trackMax.y - padY;
		float curveHeight = curveBottomY - curveTopY;

		ImVec2 mousePos = ImGui::GetMousePos();
		bool snapToGrid = !io.KeyShift && context.state.timelineGrid > 0.0;

		// the raw pair is what drag deltas are measured against; snapping or clamping the
		// anchor would fold the cursor's offset from the grabbed point back into the delta
		double mouseBeatRaw = (mousePos.x - winPos.x) / context.state.pixelsPerBeat;
		double mouseBeat = mouseBeatRaw;
		if (snapToGrid)
			mouseBeat = round(mouseBeat / context.state.timelineGrid) * context.state.timelineGrid;
		if (mouseBeat < 0)
			mouseBeat = 0;

		float mouseNormY = (curveBottomY - mousePos.y) / curveHeight;
		float mouseValRaw = minVal + mouseNormY * range;

		// segment whose tension handle the mouse is over, resolved while the curve is drawn
		int hoveredTensionIdx = -1;

		// a point's fill says whether it is selected, and the click that decides that has
		// not been handled by the time the curve is drawn. so the draw pass works out the
		// geometry only and parks it here; the circles are emitted after the interactions
		// below, reading this frame's selection. the positions are still the ones the
		// curve was drawn from, so a dragged point never leads its own curve by a frame
		struct DeferredPoint {
			ImVec2 pos;
			int index;
			bool hovered;
		};
		std::vector<DeferredPoint> deferredPoints;

		// draw curve
		if (curve->points.empty()) {
			float norm = (t->mSelectedAutomationParam->value - minVal) / range;
			float yLine = curveBottomY - norm * curveHeight;

			// dotted line when no automation points exist
			float dashSize = 8.0f;
			float gapSize = 8.0f;
			float step = dashSize + gapSize;

			// clip
			// visible x is roughly: winPos.x + scrollX to winPos.x + viewWidth + scrollX
			float visMinX = winPos.x + scrollX;
			float visMaxX = winPos.x + viewWidth + scrollX;

			float startX = std::max(trackMin.x, visMinX);
			float endX = std::min(trackMax.x, visMaxX);

			// align the loop start to the dashed pattern relative to the track start
			// should prevent dashes from jittering while scrolling
			float offset = std::fmod(startX - trackMin.x, step);
			float currX = startX - offset;

			for (; currX < endX; currX += step) {
				float x1 = std::max(currX, trackMin.x);
				float x2 = std::min(currX + dashSize, endX);

				if (x2 > x1)
					drawList->AddLine(ImVec2(x1, yLine), ImVec2(x2, yLine), Theme::WithAlpha(th.playhead, 150), 2.0f);
			}
		} else {
			// first segment
			if (!curve->points.empty()) {
				float val = curve->points[0].value;
				float norm = (val - minVal) / range;
				float py = curveBottomY - norm * curveHeight;
				float px = winPos.x + (float)(curve->points[0].beat * context.state.pixelsPerBeat);
				drawList->AddLine(ImVec2(trackMin.x, py), ImVec2(px, py), th.automationLine, 2.0f);
			}

			// bezier segments
			for (size_t pIdx = 0; pIdx < curve->points.size() - 1; ++pIdx) {
				auto& p1 = curve->points[pIdx];
				auto& p2 = curve->points[pIdx + 1];

				float x1 = winPos.x + (float)(p1.beat * context.state.pixelsPerBeat);

				// optimization: cull invisible segments
				float x2 = winPos.x + (float)(p2.beat * context.state.pixelsPerBeat);
				if (x2 < winPos.x + scrollX || x1 > winPos.x + viewWidth + scrollX)
					continue;

				float y1 = curveBottomY - ((p1.value - minVal) / range) * curveHeight;
				float y2 = curveBottomY - ((p2.value - minVal) / range) * curveHeight;

				const int segments = 24;
				ImVec2 prevPt(x1, y1);

				double tension = p1.tension;
				if (tension > 0.99f)
					tension = 0.99f;
				if (tension < -0.99f)
					tension = -0.99f;
				double exponent = std::pow(10.0, std::abs((double)tension));

				for (int s = 1; s <= segments; ++s) {
					double t = (double)s / (double)segments;
					double curvedT = t;
					if (std::abs(tension) > 0.001f) {
						if (tension > 0.0)
							curvedT = 1.0 - std::pow(1.0 - t, exponent);
						else
							curvedT = std::pow(t, exponent);
					}
					float curX = x1 + (float)t * (x2 - x1);
					float curY = y1 + (float)curvedT * (y2 - y1);
					drawList->AddLine(prevPt, ImVec2(curX, curY), th.automationLine, 2.0f);
					prevPt = ImVec2(curX, curY);
				}

				// a tension handle only means something on a segment that both slopes and has
				// room for it. a vertical (90 degree) run leaves the knob sitting on top of its
				// own points, and a flat one has no curve to shape at all
				const float kTensionMinSpanPx = 14.0f;
				const float kTensionMinRisePx = 3.0f;
				bool tensionHandleVisible = (x2 - x1) > kTensionMinSpanPx && std::abs(y2 - y1) > kTensionMinRisePx;

				if (tensionHandleVisible) {
					// draw tension handle
					double midT = 0.5;
					double midCurvedT = midT;
					if (std::abs(tension) > 0.001f) {
						if (tension > 0.0)
							midCurvedT = 1.0 - std::pow(1.0 - midT, exponent);
						else
							midCurvedT = std::pow(midT, exponent);
					}
					float midX = x1 + (float)midT * (x2 - x1);
					float midY = y1 + (float)midCurvedT * (y2 - y1);
					drawList->AddCircleFilled(ImVec2(midX, midY), 4.0f, th.ghost);

					float dx = mousePos.x - midX;
					float dy = mousePos.y - midY;
					bool knobHovered = (dx * dx + dy * dy < 36.0f);

					if (knobHovered) {
						drawList->AddCircle(ImVec2(midX, midY), 6.0f, th.noteBorderSelected);
						hoveredTensionIdx = (int)pIdx;
					}
					if (isTrackClicked && knobHovered) {
						interaction.autoEditBefore = curve->points; // undo baseline
						interaction.autoDragTrackIndex = trackIndex;
						interaction.autoDragPointIndex = (int)pIdx;
						interaction.autoDragIsTension = true;
						interaction.dragStartY = mousePos.y;
						interaction.dragStartVal = p1.tension;
					}
				}
			}

			// last segment
			{
				auto& pLast = curve->points.back();
				float xLast = winPos.x + (float)(pLast.beat * context.state.pixelsPerBeat);
				float yLast = curveBottomY - ((pLast.value - minVal) / range) * curveHeight;
				drawList->AddLine(ImVec2(xLast, yLast), ImVec2(trackMax.x, yLast), th.automationLine, 2.0f);
			}

			// draw points
			for (size_t pIdx = 0; pIdx < curve->points.size(); ++pIdx) {
				float val = curve->points[pIdx].value;
				float norm = (val - minVal) / range;
				float py = curveBottomY - norm * curveHeight;
				float px = winPos.x + (float)(curve->points[pIdx].beat * context.state.pixelsPerBeat);

				// cull points
				if (px < winPos.x + scrollX - 10 || px > winPos.x + viewWidth + scrollX + 10)
					continue;

				float radius = 6.0f;
				float dx = mousePos.x - px;
				float dy = mousePos.y - py;
				bool hovered = (dx * dx + dy * dy < (radius + 2) * (radius + 2));

				deferredPoints.push_back({ImVec2(px, py), (int)pIdx, hovered});
			}
		}

		// interactions
		int closestIdx = -1;
		if (isTrackHovered || isTrackActive) {
			float minDist = 15.0f;
			for (int pIdx = 0; pIdx < (int)curve->points.size(); ++pIdx) {
				float val = curve->points[pIdx].value;
				float norm = (val - minVal) / range;
				float py = curveBottomY - norm * curveHeight;
				float px = winPos.x + (float)(curve->points[pIdx].beat * context.state.pixelsPerBeat);

				float dx = mousePos.x - px;
				float dy = mousePos.y - py;
				float dist = std::sqrt(dx * dx + dy * dy);
				if (dist < minDist) {
					minDist = dist;
					closestIdx = pIdx;
				}
			}

			if (closestIdx == -1) {
				// preview new point
				float curveVal = curve->Evaluate(mouseBeat);
				float curveNorm = (curveVal - minVal) / range;
				float curveY = curveBottomY - curveNorm * curveHeight;

				if (std::abs(mousePos.y - curveY) < 15.0f) {
					float px = winPos.x + (float)(mouseBeat * context.state.pixelsPerBeat);
					drawList->AddCircleFilled(ImVec2(px, curveY), 5.0f, Theme::WithAlpha(th.text, 100));

					if (isTrackClicked && !interaction.autoDragIsTension) {
						interaction.autoEditBefore = curve->points; // undo baseline (before add)
						t->AddAutomationPoint(t->mSelectedAutomationParam, mouseBeat, curveVal);
						// find the index of the newly added point
						for (int k = 0; k < (int)curve->points.size(); ++k) {
							if (std::abs(curve->points[k].beat - mouseBeat) < 0.0001) {
								interaction.autoDragTrackIndex = trackIndex;
								interaction.autoDragPointIndex = k;
								interaction.autoDragIsTension = false;
								for (auto& p : curve->points)
									p.selected = false;
								curve->points[k].selected = true;
								interaction.autoDragInitialStates.clear();
								interaction.autoDragInitialStates[k] = {curve->points[k].beat, curve->points[k].value};
								interaction.autoDragAnchorBeat = mouseBeatRaw;
								interaction.autoDragAnchorVal = mouseValRaw;
								break;
							}
						}
					}
				}
			}

			if (isTrackClicked && !interaction.autoDragIsTension) {
				if (closestIdx != -1) {
					interaction.autoEditBefore = curve->points; // undo baseline (before move)
					if (io.KeyCtrl) {
						curve->points[closestIdx].selected = !curve->points[closestIdx].selected;
					} else if (io.KeyShift) {
						curve->points[closestIdx].selected = true;
					} else {
						if (!curve->points[closestIdx].selected) {
							for (auto& p : curve->points)
								p.selected = false;
							curve->points[closestIdx].selected = true;
						}
					}
					interaction.autoDragTrackIndex = trackIndex;
					interaction.autoDragPointIndex = closestIdx;
					interaction.autoDragIsTension = false;
					interaction.autoDragInitialStates.clear();
					for (int k = 0; k < (int)curve->points.size(); ++k) {
						if (curve->points[k].selected) {
							interaction.autoDragInitialStates[k] = {curve->points[k].beat, curve->points[k].value};
						}
					}
					interaction.autoDragAnchorBeat = mouseBeatRaw;
					interaction.autoDragAnchorVal = mouseValRaw;
				} else {
					// marquee start
					float curveVal = curve->Evaluate(mouseBeat);
					float curveNorm = (curveVal - minVal) / range;
					float curveY = curveBottomY - curveNorm * curveHeight;
					if (std::abs(mousePos.y - curveY) >= 15.0f) {
						interaction.autoMarqueeActive = true;
						interaction.autoMarqueeStartBeat = mouseBeat;
						interaction.autoMarqueeEndBeat = mouseBeat;
						if (!io.KeyShift && !io.KeyCtrl) {
							for (auto& p : curve->points)
								p.selected = false;
						}
					}
				}
			}

			if (isTrackRightClicked) {
				if (hoveredTensionIdx != -1) {
					interaction.autoContextTensionIndex = hoveredTensionIdx;
					ImGui::OpenPopup("AutomationTensionContext");
				} else {
					if (closestIdx != -1) {
						if (!curve->points[closestIdx].selected) {
							for (auto& p : curve->points)
								p.selected = false;
							curve->points[closestIdx].selected = true;
						}
					}
					// remember which point the menu targets so its Beat/Value fields survive
					// across the frames the popup is open, and where the click landed so Paste
					// has an anchor once the cursor has moved onto the popup itself
					interaction.autoContextPointIndex = closestIdx;
					interaction.autoContextBeat = mouseBeat;
					ImGui::OpenPopup("AutomationContext");
				}
			}
		}

		if (interaction.autoMarqueeActive) {
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				// the selection is a time range: it spans the whole lane and only its horizontal
				// bounds decide what is caught, so dragging across a curve can't miss a point
				// that happens to sit above or below the cursor. both edges ride the grid (hold
				// shift to place them freely), same as everything else on the timeline
				interaction.autoMarqueeEndBeat = mouseBeat;
				double selMinBeat = std::min(interaction.autoMarqueeStartBeat, interaction.autoMarqueeEndBeat);
				double selMaxBeat = std::max(interaction.autoMarqueeStartBeat, interaction.autoMarqueeEndBeat);

				ImVec2 rMin(winPos.x + (float)(selMinBeat * context.state.pixelsPerBeat), trackMin.y);
				ImVec2 rMax(winPos.x + (float)(selMaxBeat * context.state.pixelsPerBeat), trackMax.y);
				drawList->AddRectFilled(rMin, rMax, th.selectionFill);
				drawList->AddRect(rMin, rMax, th.selectionStroke);

				for (int k = 0; k < (int)curve->points.size(); ++k) {
					double beat = curve->points[k].beat;
					if (beat >= selMinBeat - AutomationEdits::kBeatEpsilon && beat <= selMaxBeat + AutomationEdits::kBeatEpsilon)
						curve->points[k].selected = true;
				}
			} else {
				interaction.autoMarqueeActive = false;
			}
		}

		// deferred points, painted over the curve and the marquee wash now that this
		// frame's clicks have been applied - so a point lights up the moment it is
		// grabbed rather than one frame afterwards
		for (const auto& dp : deferredPoints) {
			if (dp.index < 0 || dp.index >= (int)curve->points.size())
				continue; // the interactions above can add or remove points
			bool selected = curve->points[dp.index].selected;
			const float radius = 6.0f;

			drawList->AddCircleFilled(dp.pos, radius, selected ? th.automationPointSelected : th.automationPoint);
			if (dp.hovered || selected)
				drawList->AddCircle(dp.pos, radius + 2, Theme::WithAlpha(th.automationPointSelected, 200), 0, 2.0f);
			else
				drawList->AddCircle(dp.pos, radius, th.divider);
		}

		// context menu logic
		if (ImGui::BeginPopup("AutomationContext")) {
			std::vector<AutomationPoint> menuBefore = curve->points; // undo baseline

			// exact numeric entry for the right-clicked point. the commit is deferred to
			// IsItemDeactivatedAfterEdit so a change lands once (one undo entry, captured by
			// the menuBefore diff below) instead of per keystroke -- InputScalar asserts on
			// ImGuiInputTextFlags_EnterReturnsTrue and this is its documented replacement
			int ci = interaction.autoContextPointIndex;
			if (ci >= 0 && ci < (int)curve->points.size()) {
				double editBeat = curve->points[ci].beat;
				float editValue = curve->points[ci].value;
				bool applied = false;

				ImGui::SetNextItemWidth(110 * context.state.mainScale);
				ImGui::InputDouble("Beat", &editBeat, 0.0, 0.0, "%.3f");
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					curve->points[ci].beat = editBeat < 0.0 ? 0.0 : editBeat;
					applied = true;
				}
				ImGui::SetNextItemWidth(110 * context.state.mainScale);
				ImGui::InputFloat("Value", &editValue, 0.0f, 0.0f, "%.3f");
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					curve->points[ci].value = std::clamp(editValue, minVal, maxVal);
					applied = true;
				}
				ImGui::TextDisabled("range %.2f .. %.2f", minVal, maxVal);

				if (applied) {
					// a beat edit can reorder the curve; re-sort then re-find this point by identity
					double wb = curve->points[ci].beat;
					float wv = curve->points[ci].value;
					t->SortAutomationPoints(t->mSelectedAutomationParam);
					for (int k = 0; k < (int)curve->points.size(); ++k) {
						if (curve->points[k].beat == wb && curve->points[k].value == wv) {
							interaction.autoContextPointIndex = k;
							break;
						}
					}
				}
				ImGui::Separator();
			}

			if (ImGui::Selectable("Copy\tCtrl+C"))
				context.state.automationClipboard = AutomationEdits::CopySelection(curve->points);
			if (ImGui::Selectable("Paste\tCtrl+V", false, context.state.automationClipboard.empty() ? ImGuiSelectableFlags_Disabled : 0))
				AutomationEdits::PasteAt(curve->points, context.state.automationClipboard, interaction.autoContextBeat, minVal, maxVal);
			if (ImGui::Selectable("Duplicate\tCtrl+D"))
				AutomationEdits::DuplicateSelection(curve->points, context.state.timelineGrid);
			if (ImGui::Selectable("Delete"))
				AutomationEdits::DeleteSelected(curve->points);
			ImGui::Separator();
			if (ImGui::Selectable("Flip Vertical")) {
				float minV = 1e9f, maxV = -1e9f;
				for (const auto& p : curve->points) {
					if (p.selected) {
						if (p.value < minV)
							minV = p.value;
						if (p.value > maxV)
							maxV = p.value;
					}
				}
				float mid = (minV + maxV) * 0.5f;
				for (auto& p : curve->points) {
					if (p.selected) {
						p.value = mid - (p.value - mid);
					}
				}
			}
			if (ImGui::Selectable("Flip Horizontal")) {
				double minB = 1e9, maxB = -1e9;
				for (const auto& p : curve->points) {
					if (p.selected) {
						if (p.beat < minB)
							minB = p.beat;
						if (p.beat > maxB)
							maxB = p.beat;
					}
				}
				double mid = (minB + maxB) * 0.5;
				for (auto& p : curve->points) {
					if (p.selected) {
						p.beat = mid - (p.beat - mid);
					}
				}
				t->SortAutomationPoints(t->mSelectedAutomationParam);
			}
			// record any curve mutation the menu performed as one undo step
			if (trackPtr && !pointsEqual(menuBefore, curve->points)) {
				context.undoManager.Push(std::make_unique<AutomationEditAction>(
					project, trackPtr, t->mSelectedAutomationParam, menuBefore, curve->points));
			}
			ImGui::EndPopup();
		}

		// a tension handle has no beat or value of its own, so it gets its own menu rather
		// than sharing the point one
		if (ImGui::BeginPopup("AutomationTensionContext")) {
			std::vector<AutomationPoint> menuBefore = curve->points; // undo baseline

			int ti = interaction.autoContextTensionIndex;
			if (ti >= 0 && ti < (int)curve->points.size()) {
				float editTension = curve->points[ti].tension;

				ImGui::SetNextItemWidth(110 * context.state.mainScale);
				ImGui::InputFloat("Tension", &editTension, 0.0f, 0.0f, "%.3f");
				if (ImGui::IsItemDeactivatedAfterEdit())
					curve->points[ti].tension = std::clamp(editTension, -0.99f, 0.99f);
				ImGui::TextDisabled("range -0.99 .. 0.99");
				ImGui::Separator();
				if (ImGui::Selectable("Reset To Linear"))
					curve->points[ti].tension = 0.0f;
			} else {
				ImGui::TextDisabled("no segment");
			}

			if (trackPtr && !pointsEqual(menuBefore, curve->points)) {
				context.undoManager.Push(std::make_unique<AutomationEditAction>(
					project, trackPtr, t->mSelectedAutomationParam, menuBefore, curve->points));
			}
			ImGui::EndPopup();
		}

		// clipboard shortcuts. they live here rather than in Editor's global handler because the
		// selection and its time range are lane-local state, and a lane only reacts when it is
		// the one holding a selection -- so with one lane open this behaves like a global binding
		bool hasSelection = false;
		for (const auto& p : curve->points) {
			if (p.selected) {
				hasSelection = true;
				break;
			}
		}

		bool keysGoHere = io.KeyCtrl && !io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (keysGoHere) {
			std::vector<AutomationPoint> keyBefore = curve->points; // undo baseline

			if (hasSelection && ImGui::IsKeyPressed(ImGuiKey_C, false))
				context.state.automationClipboard = AutomationEdits::CopySelection(curve->points);
			if (hasSelection && ImGui::IsKeyPressed(ImGuiKey_D, false))
				AutomationEdits::DuplicateSelection(curve->points, context.state.timelineGrid);
			if (!context.state.automationClipboard.empty() && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
				// no cursor to aim at, so paste lands just past the selection it would otherwise
				// overwrite -- or at the playhead when nothing is selected
				double anchorBeat = 0.0;
				if (hasSelection) {
					for (const auto& p : curve->points) {
						if (p.selected)
							anchorBeat = std::max(anchorBeat, p.beat);
					}
				} else if (project) {
					Transport& tp = project->GetTransport();
					anchorBeat = (double)tp.GetPosition() / tp.GetSampleRate() * (tp.GetBpm() / 60.0);
				}
				AutomationEdits::PasteAt(curve->points, context.state.automationClipboard, anchorBeat, minVal, maxVal);
			}

			if (trackPtr && !pointsEqual(keyBefore, curve->points)) {
				context.undoManager.Push(std::make_unique<AutomationEditAction>(
					project, trackPtr, t->mSelectedAutomationParam, keyBefore, curve->points));
			}
		}

		// handle dragging logic
		if (interaction.autoDragTrackIndex == trackIndex && interaction.autoDragPointIndex != -1 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			if (interaction.autoDragIsTension) {
				float deltaY = (interaction.dragStartY - mousePos.y) / 100.0f;
				float directionMult = 1.0f;
				if (interaction.autoDragPointIndex < (int)curve->points.size() - 1) {
					float v1 = curve->points[interaction.autoDragPointIndex].value;
					float v2 = curve->points[interaction.autoDragPointIndex + 1].value;
					if (v2 < v1)
						directionMult = -1.0f;
				}
				if (interaction.autoDragPointIndex < (int)curve->points.size()) {
					float newVal = interaction.dragStartVal + (deltaY * directionMult);
					newVal = std::clamp(newVal, -0.99f, 0.99f);
					curve->points[interaction.autoDragPointIndex].tension = newVal;
					ImGui::SetTooltip("Tension: %.2f", newVal);
				}
			} else {
				auto it = interaction.autoDragInitialStates.find(interaction.autoDragPointIndex);
				if (it != interaction.autoDragInitialStates.end()) {
					double origBeat = it->second.first;

					// travel is measured from where the mouse was grabbed, so a point keeps its
					// offset from the cursor. only the grabbed point snaps; the rest of the
					// selection follows by the same delta
					double newPrimaryBeat = origBeat + (mouseBeatRaw - interaction.autoDragAnchorBeat);
					if (snapToGrid)
						newPrimaryBeat = round(newPrimaryBeat / context.state.timelineGrid) * context.state.timelineGrid;
					if (newPrimaryBeat < 0)
						newPrimaryBeat = 0;

					double deltaBeat = newPrimaryBeat - origBeat;
					float deltaVal = mouseValRaw - interaction.autoDragAnchorVal;
					for (auto& pair : interaction.autoDragInitialStates) {
						int idx = pair.first;
						if (idx < (int)curve->points.size()) {
							double newB = pair.second.first + deltaBeat;
							float newV = pair.second.second + deltaVal;
							if (newB < 0)
								newB = 0;
							newV = std::clamp(newV, minVal, maxVal);
							curve->points[idx].beat = newB;
							curve->points[idx].value = newV;
						}
					}
					if (interaction.autoDragPointIndex < (int)curve->points.size()) {
						ImGui::SetTooltip("Beat: %.2f\nValue: %.2f", curve->points[interaction.autoDragPointIndex].beat, curve->points[interaction.autoDragPointIndex].value);
					}
				}
			}
		} else if (interaction.autoDragTrackIndex == trackIndex && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			if (!interaction.autoDragIsTension && interaction.autoDragPointIndex != -1) {
				t->SortAutomationPoints(t->mSelectedAutomationParam);
			}
			// commit the whole add/move/tension gesture as one undo step
			if (trackPtr && interaction.autoDragPointIndex != -1 && !pointsEqual(interaction.autoEditBefore, curve->points)) {
				context.undoManager.Push(std::make_unique<AutomationEditAction>(
					project, trackPtr, t->mSelectedAutomationParam, interaction.autoEditBefore, curve->points));
			}
			interaction.autoDragPointIndex = -1;
			interaction.autoDragTrackIndex = -1;
			interaction.autoDragIsTension = false;
			interaction.autoDragInitialStates.clear();
		}
	}
}
