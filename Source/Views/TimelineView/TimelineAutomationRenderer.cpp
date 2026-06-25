#include "PrecompHeader.h"
#include "TimelineAutomationRenderer.h"
#include "TimelineClipRenderer.h"
#include "TimelineUtils.h"
#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include <algorithm>
#include <cmath>

void TimelineAutomationRenderer::Render(EditorContext& context, TimelineInteractionState& interaction,
										Track* t, int trackIndex,
										const ImVec2& winPos, float contentWidth, float viewWidth, float scrollX, float yPos) {

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImGuiIO& io = ImGui::GetIO();

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
			ImU32 ghostBgColor = IM_COL32(40, 40, 40, 100);
			ImU32 ghostWaveColor = IM_COL32(30, 70, 30, 60);
			ImU32 ghostMIDIColor = IM_COL32(200, 200, 200, 50);

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

		ImVec2 mousePos = ImGui::GetMousePos();
		double mouseBeat = (mousePos.x - winPos.x) / context.state.pixelsPerBeat;

		if (!io.KeyShift && context.state.timelineGrid > 0.0) {
			mouseBeat = round(mouseBeat / context.state.timelineGrid) * context.state.timelineGrid;
		}
		if (mouseBeat < 0)
			mouseBeat = 0;

		float mouseNormY = (trackMax.y - mousePos.y) / (trackMax.y - trackMin.y);
		float mouseVal = minVal + mouseNormY * range;
		mouseVal = std::clamp(mouseVal, minVal, maxVal);

		// draw curve
		if (curve->points.empty()) {
			float norm = (t->mSelectedAutomationParam->value - minVal) / range;
			float yLine = trackMax.y - norm * (trackMax.y - trackMin.y);

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
					drawList->AddLine(ImVec2(x1, yLine), ImVec2(x2, yLine), IM_COL32(255, 50, 50, 150), 2.0f);
			}
		} else {
			// first segment
			if (!curve->points.empty()) {
				float val = curve->points[0].value;
				float norm = (val - minVal) / range;
				float py = trackMax.y - norm * (trackMax.y - trackMin.y);
				float px = winPos.x + (float)(curve->points[0].beat * context.state.pixelsPerBeat);
				drawList->AddLine(ImVec2(trackMin.x, py), ImVec2(px, py), IM_COL32(255, 100, 100, 200), 2.0f);
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

				float y1 = trackMax.y - ((p1.value - minVal) / range) * (trackMax.y - trackMin.y);
				float y2 = trackMax.y - ((p2.value - minVal) / range) * (trackMax.y - trackMin.y);

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
					drawList->AddLine(prevPt, ImVec2(curX, curY), IM_COL32(255, 100, 100, 200), 2.0f);
					prevPt = ImVec2(curX, curY);
				}

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
				drawList->AddCircleFilled(ImVec2(midX, midY), 4.0f, IM_COL32(200, 200, 255, 200));

				float dx = mousePos.x - midX;
				float dy = mousePos.y - midY;
				bool knobHovered = (dx * dx + dy * dy < 36.0f);

				if (knobHovered) {
					drawList->AddCircle(ImVec2(midX, midY), 6.0f, IM_COL32(255, 255, 255, 255));
				}
				if (isTrackClicked && knobHovered) {
					interaction.autoDragTrackIndex = trackIndex;
					interaction.autoDragPointIndex = (int)pIdx;
					interaction.autoDragIsTension = true;
					interaction.dragStartY = mousePos.y;
					interaction.dragStartVal = p1.tension;
				}
			}

			// last segment
			{
				auto& pLast = curve->points.back();
				float xLast = winPos.x + (float)(pLast.beat * context.state.pixelsPerBeat);
				float yLast = trackMax.y - ((pLast.value - minVal) / range) * (trackMax.y - trackMin.y);
				drawList->AddLine(ImVec2(xLast, yLast), ImVec2(trackMax.x, yLast), IM_COL32(255, 100, 100, 200), 2.0f);
			}

			// draw points
			for (size_t pIdx = 0; pIdx < curve->points.size(); ++pIdx) {
				float val = curve->points[pIdx].value;
				float norm = (val - minVal) / range;
				float py = trackMax.y - norm * (trackMax.y - trackMin.y);
				float px = winPos.x + (float)(curve->points[pIdx].beat * context.state.pixelsPerBeat);

				// cull points
				if (px < winPos.x + scrollX - 10 || px > winPos.x + viewWidth + scrollX + 10)
					continue;

				ImU32 pointCol = curve->points[pIdx].selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 50, 255);
				float radius = 6.0f;
				float dx = mousePos.x - px;
				float dy = mousePos.y - py;
				bool hovered = (dx * dx + dy * dy < (radius + 2) * (radius + 2));

				drawList->AddCircleFilled(ImVec2(px, py), radius, pointCol);
				if (hovered || curve->points[pIdx].selected) {
					drawList->AddCircle(ImVec2(px, py), radius + 2, IM_COL32(255, 255, 255, 200), 0, 2.0f);
				} else {
					drawList->AddCircle(ImVec2(px, py), radius, IM_COL32(0, 0, 0, 255));
				}
			}
		}

		// interactions
		int closestIdx = -1;
		if (isTrackHovered || isTrackActive) {
			float minDist = 15.0f;
			for (int pIdx = 0; pIdx < (int)curve->points.size(); ++pIdx) {
				float val = curve->points[pIdx].value;
				float norm = (val - minVal) / range;
				float py = trackMax.y - norm * (trackMax.y - trackMin.y);
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
				float curveY = trackMax.y - curveNorm * (trackMax.y - trackMin.y);

				if (std::abs(mousePos.y - curveY) < 15.0f) {
					float px = winPos.x + (float)(mouseBeat * context.state.pixelsPerBeat);
					drawList->AddCircleFilled(ImVec2(px, curveY), 5.0f, IM_COL32(255, 255, 255, 100));

					if (isTrackClicked && !interaction.autoDragIsTension) {
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
								break;
							}
						}
					}
				}
			}

			if (isTrackClicked && !interaction.autoDragIsTension) {
				if (closestIdx != -1) {
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
				} else {
					// marquee start
					float curveVal = curve->Evaluate(mouseBeat);
					float curveNorm = (curveVal - minVal) / range;
					float curveY = trackMax.y - curveNorm * (trackMax.y - trackMin.y);
					if (std::abs(mousePos.y - curveY) >= 15.0f) {
						interaction.autoMarqueeActive = true;
						interaction.autoMarqueeStart = mousePos;
						interaction.autoMarqueeCurrent = mousePos;
						if (!io.KeyShift && !io.KeyCtrl) {
							for (auto& p : curve->points)
								p.selected = false;
						}
					}
				}
			}

			if (isTrackRightClicked) {
				if (closestIdx != -1) {
					if (!curve->points[closestIdx].selected) {
						for (auto& p : curve->points)
							p.selected = false;
						curve->points[closestIdx].selected = true;
					}
				}
				ImGui::OpenPopup("AutomationContext");
			}
		}

		if (interaction.autoMarqueeActive) {
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				interaction.autoMarqueeCurrent = ImGui::GetMousePos();
				ImVec2 rMin(std::min(interaction.autoMarqueeStart.x, interaction.autoMarqueeCurrent.x), std::min(interaction.autoMarqueeStart.y, interaction.autoMarqueeCurrent.y));
				ImVec2 rMax(std::max(interaction.autoMarqueeStart.x, interaction.autoMarqueeCurrent.x), std::max(interaction.autoMarqueeStart.y, interaction.autoMarqueeCurrent.y));
				drawList->AddRectFilled(rMin, rMax, IM_COL32(200, 200, 255, 50));
				drawList->AddRect(rMin, rMax, IM_COL32(200, 200, 255, 200));

				for (int k = 0; k < (int)curve->points.size(); ++k) {
					float val = curve->points[k].value;
					float norm = (val - minVal) / range;
					float py = trackMax.y - norm * (trackMax.y - trackMin.y);
					float px = winPos.x + (float)(curve->points[k].beat * context.state.pixelsPerBeat);

					if (px >= rMin.x && px <= rMax.x && py >= rMin.y && py <= rMax.y) {
						curve->points[k].selected = true;
					}
				}
			} else {
				interaction.autoMarqueeActive = false;
			}
		}

		// context menu logic
		if (ImGui::BeginPopup("AutomationContext")) {
			if (ImGui::Selectable("Copy")) {
				context.state.automationClipboard.clear();
				std::vector<AutomationPoint> sortedSel;
				for (const auto& p : curve->points) {
					if (p.selected)
						sortedSel.push_back(p);
				}
				if (!sortedSel.empty()) {
					double baseBeat = sortedSel[0].beat;
					for (auto& p : sortedSel) {
						p.beat -= baseBeat;
						context.state.automationClipboard.push_back(p);
					}
				}
			}
			if (ImGui::Selectable("Paste", false, context.state.automationClipboard.empty() ? ImGuiSelectableFlags_Disabled : 0)) {
				for (const auto& clipPt : context.state.automationClipboard) {
					t->AddAutomationPoint(t->mSelectedAutomationParam, mouseBeat + clipPt.beat, clipPt.value);
				}
				t->SortAutomationPoints(t->mSelectedAutomationParam);
			}
			if (ImGui::Selectable("Duplicate")) {
				std::vector<AutomationPoint> newPoints;
				double minB = 1e9, maxB = -1e9;
				for (const auto& p : curve->points) {
					if (p.selected) {
						if (p.beat < minB)
							minB = p.beat;
						if (p.beat > maxB)
							maxB = p.beat;
					}
				}
				double duration = (maxB - minB);
				if (duration < context.state.timelineGrid)
					duration = context.state.timelineGrid;

				for (const auto& p : curve->points) {
					if (p.selected) {
						AutomationPoint np = p;
						np.beat += duration;
						newPoints.push_back(np);
					}
				}
				for (const auto& np : newPoints) {
					t->AddAutomationPoint(t->mSelectedAutomationParam, np.beat, np.value);
				}
				t->SortAutomationPoints(t->mSelectedAutomationParam);
			}
			if (ImGui::Selectable("Delete")) {
				for (int k = (int)curve->points.size() - 1; k >= 0; --k) {
					if (curve->points[k].selected) {
						curve->points.erase(curve->points.begin() + k);
					}
				}
			}
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
			ImGui::EndPopup();
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
					float origVal = it->second.second;
					double deltaBeat = mouseBeat - origBeat;
					float deltaVal = mouseVal - origVal;
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
			interaction.autoDragPointIndex = -1;
			interaction.autoDragTrackIndex = -1;
			interaction.autoDragIsTension = false;
			interaction.autoDragInitialStates.clear();
		}
	}
}
