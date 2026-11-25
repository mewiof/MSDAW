#include "PrecompHeader.h"
#include "SliderParameter.h"
#include "Theme.h"

bool SliderParameter::Draw() {
	bool changed = false;
	ImGui::PushID(this);
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IsSelected() ? Theme::Instance().accent : Theme::Instance().textMuted), "%s", name.c_str());

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

		// the same face as a compact box, always with its level fill
		DrawValueBox(ImGui::GetWindowDrawList(), pos, size, "%.2f", true, isHovered, isActive);

		changed |= HandleCommonInteractions();
	}

	ImGui::PopID();
	return changed;
}

float SliderParameter::FillOrigin() const {
	return mBipolar ? NormalizedZero() : 0.0f;
}
