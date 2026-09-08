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
		case ImGuiKnobVariant_PercentBipolar:
			snprintf(buffer, bufferSize, "%.0f%%", value);
			break;
		case ImGuiKnobVariant_Hertz:
			// an LFO lives below 10 Hz, where one decimal rounds most of its travel to the
			// same reading. the extra digit only ever appears down there
			if (value >= 1000.0f)
				snprintf(buffer, bufferSize, "%.2f kHz", value / 1000.0f);
			else if (value >= 10.0f)
				snprintf(buffer, bufferSize, "%.1f Hz", value);
			else
				snprintf(buffer, bufferSize, "%.2f Hz", value);
			break;
		case ImGuiKnobVariant_Decibel:
		case ImGuiKnobVariant_DecibelBipolar:
			if (value > -70.0f)
				snprintf(buffer, bufferSize, "%+.1f dB", value);
			else
				snprintf(buffer, bufferSize, "-inf dB");
			break;
		case ImGuiKnobVariant_Milliseconds:
			if (value >= 100.0f)
				snprintf(buffer, bufferSize, "%.0f ms", value);
			else if (value >= 10.0f)
				snprintf(buffer, bufferSize, "%.1f ms", value);
			else
				snprintf(buffer, bufferSize, "%.2f ms", value);
			break;
		case ImGuiKnobVariant_Integer:
			snprintf(buffer, bufferSize, "%.0f", value);
			break;
		case ImGuiKnobVariant_Degrees:
			// the degree sign is Latin-1, which the app's font range already covers
			snprintf(buffer, bufferSize, "%.0f°", value);
			break;
		case ImGuiKnobVariant_Linear:
		case ImGuiKnobVariant_LinearBipolar:
		default:
			snprintf(buffer, bufferSize, "%.2f", value);
			break;
		}
	}
} //namespace

float KnobParameter::NormalizedFromValue() const {
	if (variant == ImGuiKnobVariant_Hertz)
		return LogToLinear(value, minValue, maxValue);
	return ContinuousParameter::NormalizedFromValue();
}

void KnobParameter::SetValueFromNormalized(float t) {
	if (variant == ImGuiKnobVariant_Hertz) {
		value = LinearToLog(std::clamp(t, 0.0f, 1.0f), minValue, maxValue);
	} else {
		ContinuousParameter::SetValueFromNormalized(t);
		// a count has no meaning between two of its steps, so the dial lands on one. the
		// snap happens here rather than at the read site so the printed value, the dial
		// angle and what the audio thread uses can never disagree
		if (variant == ImGuiKnobVariant_Integer)
			value = std::round(value);
	}
}

void KnobParameter::FormatValue(char* buffer, size_t bufferSize, const char* valueFmt) const {
	// an explicit format still wins: a caller that asked for "%.0f" wants that
	if (valueFmt)
		ContinuousParameter::FormatValue(buffer, bufferSize, valueFmt);
	else
		FormatKnobValue(buffer, bufferSize, value, variant);
}

float KnobParameter::SizedHeight(float radius) {
	return ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().ItemInnerSpacing.y * 2.0f + radius * 2.0f;
}

float KnobParameter::RadiusForHeight(float height) {
	const float radius = (height - ImGui::GetTextLineHeight() * 2.0f - ImGui::GetStyle().ItemInnerSpacing.y * 2.0f) * 0.5f;
	// under this the dial is a smudge and the block would read better as a value box
	return radius >= 7.0f ? radius : 0.0f;
}

bool KnobParameter::Draw() {
	return DrawSized(kDefaultRadius);
}

bool KnobParameter::DrawSized(float radius, float width, const char* label) {
	bool changed = false;
	ImGui::PushID(this);

	ImGuiID currentID = ImGui::GetID("##KnobBtn");
	CheckTypingStart(currentID);

	ImGuiStyle& style = ImGui::GetStyle();
	const float lineHeight = ImGui::GetTextLineHeight();

	char valBuffer[64];
	FormatValue(valBuffer, sizeof(valBuffer), nullptr);

	const char* labelText = label ? label : name.c_str();
	ImVec2 labelSize = ImGui::CalcTextSize(labelText);
	ImVec2 valSize = ImGui::CalcTextSize(valBuffer);

	float totalWidth = width > 0.0f ? width : std::max({radius * 2.0f, labelSize.x, valSize.x});
	float totalHeight = SizedHeight(radius);

	if (IsTyping(currentID)) {
		changed |= DrawTypingInput(currentID, totalWidth, (totalHeight - lineHeight) * 0.5f);
	} else {
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##KnobBtn", ImVec2(totalWidth, totalHeight));

		bool isActive = ImGui::IsItemActive();
		bool isHovered = ImGui::IsItemHovered() && !ImGui::IsAnyItemActive();

		if (ImGui::IsItemActivated()) {
			BeginEditGesture(); // capture value at drag start (one undo entry per drag)
			BeginDragPosition();
		}

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

				ApplyDragDelta(-deltaY * mouseSensitivity);
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

		float t = NormalizedFromValue();
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

		const float ringThickness = std::max(2.0f, radius * 0.17f);
		drawList->PathArcTo(center, radius * 0.85f, ANGLE_MIN, ANGLE_MAX, 32);
		drawList->PathStroke(colBackgroud, 0, ringThickness);

		if (variant == ImGuiKnobVariant_DecibelBipolar || variant == ImGuiKnobVariant_LinearBipolar ||
			variant == ImGuiKnobVariant_PercentBipolar) {
			float tZero = (0.0f - minValue) / (maxValue - minValue);
			float angleZero = ANGLE_MIN + (ANGLE_MAX - ANGLE_MIN) * tZero;

			if (std::abs(t - tZero) > 0.001f) {
				float a1 = std::min(angle, angleZero);
				float a2 = std::max(angle, angleZero);
				drawList->PathArcTo(center, radius * 0.85f, a1, a2, 32);
				drawList->PathStroke(colArc, 0, ringThickness);
			}
		} else if (t > 0.001f) {
			drawList->PathArcTo(center, radius * 0.85f, ANGLE_MIN, angle, 32);
			drawList->PathStroke(colArc, 0, ringThickness);
		}

		ImVec2 tickVector = ImVec2(cosf(angle), sinf(angle));
		float arcRadius = radius * 0.85f;
		float tickLen = arcRadius + 1.5f;
		drawList->AddLine(
			ImVec2(center.x + tickVector.x, center.y + tickVector.y),
			ImVec2(center.x + tickVector.x * tickLen, center.y + tickVector.y * tickLen),
			colBackgroud, ringThickness);

		ImVec2 labelPos = ImVec2(pos.x + (totalWidth - labelSize.x) * 0.5f, pos.y);
		drawList->AddText(labelPos, colText, labelText);

		ImVec2 valPos = ImVec2(pos.x + (totalWidth - valSize.x) * 0.5f, pos.y + lineHeight + style.ItemInnerSpacing.y + (radius * 2) + style.ItemInnerSpacing.y);
		ImU32 valTextCol = (isActive || isHovered) ? colArc : colText;
		drawList->AddText(valPos, valTextCol, valBuffer);

		changed |= HandleCommonInteractions();
	}

	ImGui::PopID();
	return changed;
}
