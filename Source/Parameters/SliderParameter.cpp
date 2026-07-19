#include "PrecompHeader.h"
#include "SliderParameter.h"
#include "Theme.h"

bool SliderParameter::Draw() {
	bool changed = false;
	ImGui::PushID(this);
	ImGui::Text("%s", name.c_str());

	ImGui::SetNextItemWidth(-1.0f);

	ImGuiID currentID = ImGui::GetID("##SliderBtn");
	CheckTypingStart(currentID);

	if (IsTyping(currentID)) {
		changed |= DrawTypingInput(currentID, -1.0f);
	} else {
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImVec2 size(ImGui::CalcItemWidth(), ImGui::GetFrameHeight());
		if (size.x <= 0)
			size.x = ImGui::GetContentRegionAvail().x;

		ImGui::InvisibleButton("##SliderBtn", size);

		bool isActive = ImGui::IsItemActive();
		bool isHovered = ImGui::IsItemHovered() && !ImGui::IsAnyItemActive();

		if (ImGui::IsItemActivated())
			BeginEditGesture(); // capture value at drag start (one undo entry per drag)

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
			Select();

		if (isActive) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);

			float deltaY = GetSafeMouseDeltaY();
			if (deltaY != 0.0f) {
				float range = maxValue - minValue;
				float sensitivity = range / 200.0f;
				if (ImGui::GetIO().KeyShift)
					sensitivity *= 0.1f;

				float minV = std::min(minValue, maxValue);
				float maxV = std::max(minValue, maxValue);
				value = std::clamp(value - (deltaY * sensitivity), minV, maxV);
				changed = true;
			}
			HandleInfiniteDrag();
		}

		if (ImGui::IsItemDeactivated()) {
			RestoreMousePosition();
			EndEditGesture(); // commit the drag as a single undo entry
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImU32 bgColor = ImGui::GetColorU32(isHovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
		ImU32 fillColor = ImGui::GetColorU32(isActive ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);

		drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgColor, ImGui::GetStyle().FrameRounding);

		float fraction = (value - minValue) / (maxValue - minValue);
		ImVec2 fillMax = ImVec2(pos.x + fraction * size.x, pos.y + size.y);
		drawList->AddRectFilled(pos, fillMax, fillColor, ImGui::GetStyle().FrameRounding);

		// always outline the track so it reads as a distinct control, not a flat patch
		// of panel. the last-touched parameter gets the accent ring instead, so its
		// automation target stays obvious
		const Theme& th = Theme::Instance();
		drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IsSelected() ? th.accent : th.border, ImGui::GetStyle().FrameRounding);

		char valText[32];
		snprintf(valText, sizeof(valText), "%.2f", value);
		ImVec2 textSize = ImGui::CalcTextSize(valText);
		ImVec2 textPos = ImVec2(pos.x + (size.x - textSize.x) * 0.5f, pos.y + (size.y - textSize.y) * 0.5f);
		drawList->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), valText);

		changed |= HandleCommonInteractions();
	}

	ImGui::PopID();
	return changed;
}
