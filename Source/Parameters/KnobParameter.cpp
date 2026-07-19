#include "PrecompHeader.h"
#include "KnobParameter.h"
#include "Theme.h"

namespace {
	float LinearToLog(float t, float min, float max) {
		if (min <= 0.0f || max <= 0.0f)
			return min + t * (max - min); // fallback if negative
		return min * powf(max / min, t);
	}

	float LogToLinear(float value, float min, float max) {
		if (min <= 0.0f || max <= 0.0f)
			return (value - min) / (max - min);
		return logf(value / min) / logf(max / min);
	}

	void FormatKnobValue(char* buffer, size_t bufferSize, float value, ImGuiKnobVariant variant) {
		switch (variant) {
		case ImGuiKnobVariant_Percent:
			snprintf(buffer, bufferSize, "%.0f%%", value);
			break;
		case ImGuiKnobVariant_Hertz:
			if (value >= 1000.0f)
				snprintf(buffer, bufferSize, "%.2f kHz", value / 1000.0f);
			else
				snprintf(buffer, bufferSize, "%.1f Hz", value);
			break;
		case ImGuiKnobVariant_Decibel:
		case ImGuiKnobVariant_DecibelBipolar:
			if (value > -70.0f)
				snprintf(buffer, bufferSize, "%+.1f dB", value);
			else
				snprintf(buffer, bufferSize, "-inf dB");
			break;
		case ImGuiKnobVariant_Linear:
		default:
			snprintf(buffer, bufferSize, "%.2f", value);
			break;
		}
	}
} //namespace

bool KnobParameter::Draw() {
	bool changed = false;
	ImGui::PushID(this);

	ImGuiID currentID = ImGui::GetID("##KnobBtn");
	CheckTypingStart(currentID);

	ImGuiStyle& style = ImGui::GetStyle();
	const float radius = 18.0f;
	const float lineHeight = ImGui::GetTextLineHeight();

	char valBuffer[64];
	FormatKnobValue(valBuffer, sizeof(valBuffer), value, variant);

	ImVec2 labelSize = ImGui::CalcTextSize(name.c_str());
	ImVec2 valSize = ImGui::CalcTextSize(valBuffer);

	float totalWidth = std::max({radius * 2.0f, labelSize.x, valSize.x});
	float totalHeight = (lineHeight * 2) + (style.ItemInnerSpacing.y * 2) + (radius * 2);

	if (IsTyping(currentID)) {
		changed |= DrawTypingInput(currentID, totalWidth, (totalHeight - lineHeight) * 0.5f);
	} else {
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##KnobBtn", ImVec2(totalWidth, totalHeight));

		bool isActive = ImGui::IsItemActive();
		bool isHovered = ImGui::IsItemHovered() && !ImGui::IsAnyItemActive();

		if (ImGui::IsItemActivated())
			BeginEditGesture(); // capture value at drag start (one undo entry per drag)

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			Select();
		}

		if (isActive) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);

			float deltaY = GetSafeMouseDeltaY();
			if (deltaY != 0.0f) {
				float mouseSensitivity = 0.005f;
				if (ImGui::GetIO().KeyShift)
					mouseSensitivity *= 0.1f;

				float t = (variant == ImGuiKnobVariant_Hertz) ? LogToLinear(value, minValue, maxValue) : (value - minValue) / (maxValue - minValue);
				t -= deltaY * mouseSensitivity;
				t = std::clamp(t, 0.0f, 1.0f);

				value = (variant == ImGuiKnobVariant_Hertz) ? LinearToLog(t, minValue, maxValue) : (minValue + t * (maxValue - minValue));
				changed = true;
			}
			HandleInfiniteDrag();
		}

		if (ImGui::IsItemDeactivated()) {
			RestoreMousePosition();
			EndEditGesture(); // commit the drag as a single undo entry
		}

		// visuals
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const float ANGLE_MIN = 3.14159265359f * 0.675f;
		const float ANGLE_MAX = 3.14159265359f * 2.325f;

		float knobCenterY = pos.y + lineHeight + style.ItemInnerSpacing.y + radius;
		ImVec2 center = ImVec2(pos.x + totalWidth * 0.5f, knobCenterY);

		float t = (variant == ImGuiKnobVariant_Hertz) ? LogToLinear(value, minValue, maxValue) : (value - minValue) / (maxValue - minValue);
		float angle = ANGLE_MIN + (ANGLE_MAX - ANGLE_MIN) * t;

		ImU32 colBackgroud = ImGui::GetColorU32(ImGuiCol_FrameBg);
		ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);

		ImVec4 arcColorVec = ImGui::GetStyle().Colors[ImGuiCol_PlotHistogram];
		if (isActive || isHovered) {
			arcColorVec.x = std::min(arcColorVec.x * 1.3f, 1.0f);
			arcColorVec.y = std::min(arcColorVec.y * 1.3f, 1.0f);
			arcColorVec.z = std::min(arcColorVec.z * 1.3f, 1.0f);
			arcColorVec.w = 1.0f;
		}
		ImU32 colArc = ImGui::ColorConvertFloat4ToU32(arcColorVec);

		// the last-touched parameter gets an accent ring so its automation target is obvious
		if (IsSelected())
			drawList->AddRect(pos, ImVec2(pos.x + totalWidth, pos.y + totalHeight), Theme::Instance().accent, ImGui::GetStyle().FrameRounding);

		drawList->PathArcTo(center, radius * 0.85f, ANGLE_MIN, ANGLE_MAX, 32);
		drawList->PathStroke(colBackgroud, 0, 3.0f);

		if (variant == ImGuiKnobVariant_DecibelBipolar) {
			float tZero = (0.0f - minValue) / (maxValue - minValue);
			float angleZero = ANGLE_MIN + (ANGLE_MAX - ANGLE_MIN) * tZero;

			if (std::abs(t - tZero) > 0.001f) {
				float a1 = std::min(angle, angleZero);
				float a2 = std::max(angle, angleZero);
				drawList->PathArcTo(center, radius * 0.85f, a1, a2, 32);
				drawList->PathStroke(colArc, 0, 3.0f);
			}
		} else if (t > 0.001f) {
			drawList->PathArcTo(center, radius * 0.85f, ANGLE_MIN, angle, 32);
			drawList->PathStroke(colArc, 0, 3.0f);
		}

		ImVec2 tickVector = ImVec2(cosf(angle), sinf(angle));
		float arcRadius = radius * 0.85f;
		float tickLen = arcRadius + 1.5f;
		drawList->AddLine(
			ImVec2(center.x + tickVector.x, center.y + tickVector.y),
			ImVec2(center.x + tickVector.x * tickLen, center.y + tickVector.y * tickLen),
			colBackgroud, 3.0f);

		ImVec2 labelPos = ImVec2(pos.x + (totalWidth - labelSize.x) * 0.5f, pos.y);
		drawList->AddText(labelPos, colText, name.c_str());

		ImVec2 valPos = ImVec2(pos.x + (totalWidth - valSize.x) * 0.5f, pos.y + lineHeight + style.ItemInnerSpacing.y + (radius * 2) + style.ItemInnerSpacing.y);
		ImU32 valTextCol = (isActive || isHovered) ? colArc : colText;
		drawList->AddText(valPos, valTextCol, valBuffer);

		changed |= HandleCommonInteractions();
	}

	ImGui::PopID();
	return changed;
}
