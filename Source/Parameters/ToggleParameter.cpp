#include "PrecompHeader.h"
#include "ToggleParameter.h"
#include "Widgets.h"

bool ToggleParameter::Draw() {
	ImGui::PushID(this);
	// a switch reads as a button carrying its name, lit while on, the way the reference
	// product draws one - not as a checkbox with a caption
	bool bVal = value > 0.5f;
	bool changed = Widgets::TextToggle(name.c_str(), bVal, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f));
	if (changed) {
		float oldValue = value;
		value = bVal ? 0 : 1;
		CommitEditImmediate(oldValue);
	}
	changed |= HandleCommonInteractions();
	ImGui::PopID();
	return changed;
}
