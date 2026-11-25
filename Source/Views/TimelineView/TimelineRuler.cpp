#include "PrecompHeader.h"
#include "TimelineRuler.h"
#include "Project.h"
#include "Theme.h"
#include "Widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

void TimelineRuler::Render(EditorContext& context, TimelineInteractionState& interaction,
						   const ImVec2& winPos, float contentWidth, float visibleWidth, float height, float scrollX) {
	const Theme& th = Theme::Instance();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	Project* project = context.GetProject();
	Transport* transport = project ? &project->GetTransport() : nullptr;

	// draw ruler background: a bar like the transport's, closed off from the lanes under
	// it by the same dark grout that runs between them
	drawList->AddRectFilled(winPos, ImVec2(winPos.x + contentWidth, winPos.y + height), th.bgHeader);
	drawList->AddRectFilled(ImVec2(winPos.x, winPos.y + height - 1.0f), ImVec2(winPos.x + contentWidth, winPos.y + height), th.divider);

	// calculate beat range visible
	if (visibleWidth < 1.0f)
		visibleWidth = 1920.0f;

	double startVis = scrollX / context.state.pixelsPerBeat;
	double endVis = (scrollX + visibleWidth) / context.state.pixelsPerBeat;

	double grid = context.state.timelineGrid > 0.0 ? context.state.timelineGrid : 1.0;

	int startIdx = (int)floor(startVis / grid);
	int endIdx = (int)ceil(endVis / grid);

	// a bar's tick runs from beside its number down to the bottom edge, and the shorter
	// ticks between bars take the lower quarter. the numbers are set in the caption face;
	// all of it is measured off the ruler's height and that face, so the numbers keep
	// clear of the top edge at any scale
	const float scale = context.state.mainScale;
	Widgets::PushSmallFont();
	const float labelInset = 3.0f * scale;
	const float barTickTop = std::min(labelInset + ImGui::GetTextLineHeight() * 0.5f, height * 0.5f);
	const float beatTickTop = height * 0.74f;

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

		// ticks are a pixel wide and sit on whole pixels, so they stay crisp at any zoom
		const float tickX = std::floor(x) + 0.5f;
		if (isBar) {
			drawList->AddLine(ImVec2(tickX, winPos.y + barTickTop), ImVec2(tickX, winPos.y + height), th.textDim);
			char buf[16];
			snprintf(buf, sizeof(buf), "%d", (int)(b / 4) + 1);
			drawList->AddText(ImVec2(x + 4.0f * scale, winPos.y + labelInset), th.textMuted, buf);
		} else {
			drawList->AddLine(ImVec2(tickX, winPos.y + beatTickTop), ImVec2(tickX, winPos.y + height), Theme::WithAlpha(th.textDim, 150));
		}
	}
	ImGui::PopFont();

	// loop/selection region (visuals). a brace along the top edge over a light wash: gray
	// while it is only a selection, the accent once the transport loops it
	if (context.state.selectionEnd > context.state.selectionStart) {
		float selX1 = winPos.x + (float)(context.state.selectionStart * context.state.pixelsPerBeat);
		float selX2 = winPos.x + (float)(context.state.selectionEnd * context.state.pixelsPerBeat);

		const bool looping = transport && transport->IsLoopEnabled();
		const ImU32 brace = looping ? th.accent : th.textMuted;
		const float braceHeight = std::max(3.0f, std::floor(4.0f * scale));
		drawList->AddRectFilled(ImVec2(selX1, winPos.y), ImVec2(selX2, winPos.y + height), looping ? th.accentTranslucent : th.selectionFill);
		drawList->AddRectFilled(ImVec2(selX1, winPos.y), ImVec2(selX2, winPos.y + braceHeight), brace);
		drawList->AddLine(ImVec2(std::floor(selX1) + 0.5f, winPos.y), ImVec2(std::floor(selX1) + 0.5f, winPos.y + height), brace);
		drawList->AddLine(ImVec2(std::floor(selX2) + 0.5f, winPos.y), ImVec2(std::floor(selX2) + 0.5f, winPos.y + height), brace);
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
