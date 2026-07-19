#include "PrecompHeader.h"
#include "TimelineRuler.h"
#include "Project.h"
#include "Theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

void TimelineRuler::Render(EditorContext& context, TimelineInteractionState& interaction,
						   const ImVec2& winPos, float contentWidth, float visibleWidth, float height, float scrollX) {
	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	Project* project = context.GetProject();
	Transport* transport = project ? &project->GetTransport() : nullptr;

	// draw ruler background
	drawList->AddRectFilled(winPos, ImVec2(winPos.x + contentWidth, winPos.y + height), th.bgPanel);
	drawList->AddLine(ImVec2(winPos.x, winPos.y + height), ImVec2(winPos.x + contentWidth, winPos.y + height), th.borderStrong);

	// calculate beat range visible
	if (visibleWidth < 1.0f)
		visibleWidth = 1920.0f;

	double startVis = scrollX / context.state.pixelsPerBeat;
	double endVis = (scrollX + visibleWidth) / context.state.pixelsPerBeat;

	double grid = context.state.timelineGrid > 0.0 ? context.state.timelineGrid : 1.0;

	int startIdx = (int)floor(startVis / grid);
	int endIdx = (int)ceil(endVis / grid);

	// loop to draw grid lines on ruler
	for (int i = startIdx; i <= endIdx; ++i) {
		double b = i * grid;
		float x = winPos.x + (float)(b * context.state.pixelsPerBeat);

		// safety cap
		if (x > winPos.x + contentWidth)
			break;
		if (x < winPos.x)
			continue;

		bool isBar = std::abs(fmod(b + 0.001, 4.0)) < 0.002;

		if (isBar) {
			drawList->AddLine(ImVec2(x, winPos.y + 15), ImVec2(x, winPos.y + height), th.textMuted);
			char buf[16];
			sprintf(buf, "%d", (int)(b / 4) + 1);
			drawList->AddText(ImVec2(x + 2, winPos.y), th.textMuted, buf);
		} else {
			drawList->AddLine(ImVec2(x, winPos.y + 25), ImVec2(x, winPos.y + height), th.textDim);
		}
	}

	// loop/selection region (visuals)
	if (context.state.selectionEnd > context.state.selectionStart) {
		float selX1 = winPos.x + (float)(context.state.selectionStart * context.state.pixelsPerBeat);
		float selX2 = winPos.x + (float)(context.state.selectionEnd * context.state.pixelsPerBeat);

		drawList->AddRectFilled(ImVec2(selX1, winPos.y), ImVec2(selX2, winPos.y + height), Theme::WithAlpha(th.marker, 90));
		drawList->AddLine(ImVec2(selX1, winPos.y), ImVec2(selX2, winPos.y), th.marker, 3.0f);
		drawList->AddLine(ImVec2(selX1, winPos.y), ImVec2(selX1, winPos.y + height), th.marker);
		drawList->AddLine(ImVec2(selX2, winPos.y), ImVec2(selX2, winPos.y + height), th.marker);
	}

	// ruler interaction
	ImGui::SetCursorScreenPos(ImVec2(winPos.x + scrollX, winPos.y));
	ImGui::InvisibleButton("##RulerHitbox", ImVec2(visibleWidth, height));
	bool rulerHovered = ImGui::IsItemHovered();
	bool rulerActive = ImGui::IsItemActive();

	if (rulerActive) {
		float mouseX = ImGui::GetMousePos().x;
		double beat = (mouseX - winPos.x) / context.state.pixelsPerBeat;
		if (beat < 0)
			beat = 0;
		if (context.state.timelineGrid > 0.0)
			beat = round(beat / context.state.timelineGrid) * context.state.timelineGrid;

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			interaction.selectionDragStart = beat;
			context.state.selectionStart = beat;
			context.state.selectionEnd = beat;

			if (transport) {
				int64_t newSample = (int64_t)(beat * (60.0 / transport->GetBpm()) * transport->GetSampleRate());
				transport->SetPosition(newSample);
				transport->SetLoopRange(0, 0);
			}
		} else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
			double start = std::min(interaction.selectionDragStart, beat);
			double end = std::max(interaction.selectionDragStart, beat);
			context.state.selectionStart = start;
			context.state.selectionEnd = end;

			if (transport) {
				double secStart = start * (60.0 / transport->GetBpm());
				double secEnd = end * (60.0 / transport->GetBpm());
				transport->SetLoopRange((int64_t)(secStart * transport->GetSampleRate()), (int64_t)(secEnd * transport->GetSampleRate()));
			}
		}
	}
}
