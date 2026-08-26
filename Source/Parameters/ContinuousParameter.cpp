#include "PrecompHeader.h"
#include "ContinuousParameter.h"
#include "Theme.h"

ImGuiID ContinuousParameter::s_TypingID = 0;
char ContinuousParameter::s_TextBuffer[64] = "";
bool ContinuousParameter::s_FocusNextFrame = false;
bool ContinuousParameter::s_MoveCursorToEnd = false;

float ContinuousParameter::NormalizedFromValue() const {
	const float range = maxValue - minValue;
	return range != 0.0f ? (value - minValue) / range : 0.0f;
}

void ContinuousParameter::SetValueFromNormalized(float t) {
	value = minValue + std::clamp(t, 0.0f, 1.0f) * (maxValue - minValue);
}

void ContinuousParameter::FormatValue(char* buffer, size_t bufferSize, const char* valueFmt) const {
	snprintf(buffer, bufferSize, valueFmt ? valueFmt : "%.2f", value);
}

bool ContinuousParameter::DrawCompact(float width, const char* valueFmt, bool drawFill) {
	bool changed = false;
	ImGui::PushID(this);

	ImGuiID currentID = ImGui::GetID("##CompactBtn");
	CheckTypingStart(currentID);

	if (IsTyping(currentID)) {
		changed |= DrawTypingInput(currentID, width);
	} else {
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImVec2 size(width, ImGui::GetFrameHeight());

		ImGui::InvisibleButton("##CompactBtn", size);

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
				// 200 px of travel covers the whole range, in whatever space this
				// parameter maps normalized values through
				float sensitivity = 0.005f;
				if (ImGui::GetIO().KeyShift)
					sensitivity *= 0.1f;

				SetValueFromNormalized(NormalizedFromValue() - deltaY * sensitivity);
				changed = true;
			}
			HandleInfiniteDrag();
		}

		if (ImGui::IsItemDeactivated()) {
			RestoreMousePosition();
			EndEditGesture(); // commit the drag as a single undo entry
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const Theme& th = Theme::Instance();
		ImU32 bgColor = ImGui::GetColorU32(isHovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
		drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgColor, ImGui::GetStyle().FrameRounding);
		if (drawFill) {
			float fraction = std::clamp(NormalizedFromValue(), 0.0f, 1.0f);
			ImU32 fillColor = ImGui::GetColorU32(isActive ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);
			drawList->AddRectFilled(pos, ImVec2(pos.x + fraction * size.x, pos.y + size.y), fillColor, ImGui::GetStyle().FrameRounding);
		}
		drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IsSelected() ? th.accent : th.border, ImGui::GetStyle().FrameRounding);

		char valText[32];
		FormatValue(valText, sizeof(valText), valueFmt);
		ImVec2 textSize = ImGui::CalcTextSize(valText);
		ImVec2 textPos = ImVec2(pos.x + (size.x - textSize.x) * 0.5f, pos.y + (size.y - textSize.y) * 0.5f);
		drawList->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), valText);

		changed |= HandleCommonInteractions();
	}

	ImGui::PopID();
	return changed;
}

void ContinuousParameter::CheckTypingStart(ImGuiID currentID) {
	if (IsSelected() && s_TypingID == 0) {
		bool startTyping = false;
		char initialChar = '\0';

		if (!ImGui::GetIO().WantTextInput) {
			for (int k = ImGuiKey_0; k <= ImGuiKey_9; k++) {
				if (ImGui::IsKeyPressed((ImGuiKey)k)) {
					startTyping = true;
					initialChar = '0' + (k - ImGuiKey_0);
					break;
				}
			}
			if (!startTyping) {
				for (int k = ImGuiKey_Keypad0; k <= ImGuiKey_Keypad9; k++) {
					if (ImGui::IsKeyPressed((ImGuiKey)k)) {
						startTyping = true;
						initialChar = '0' + (k - ImGuiKey_Keypad0);
						break;
					}
				}
			}
			if (!startTyping && (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))) {
				startTyping = true;
				initialChar = '-';
			}
			if (!startTyping && (ImGui::IsKeyPressed(ImGuiKey_Period) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal))) {
				startTyping = true;
				initialChar = '.';
			}
		}

		if (startTyping) {
			s_TypingID = currentID;
			s_TextBuffer[0] = initialChar;
			s_TextBuffer[1] = '\0';
			s_FocusNextFrame = true;
			s_MoveCursorToEnd = true;
		}
	}
}

bool ContinuousParameter::IsTyping(ImGuiID currentID) const {
	return s_TypingID == currentID;
}

bool ContinuousParameter::DrawTypingInput(ImGuiID currentID, float width, float yOffset) {
	bool changed = false;

	if (yOffset != 0.0f)
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + yOffset);

	if (width > 0.0f)
		ImGui::PushItemWidth(width);
	else
		ImGui::PushItemWidth(-1.0f);

	if (s_FocusNextFrame) {
		ImGui::SetKeyboardFocusHere();
		s_FocusNextFrame = false;
	}

	auto moveCursorToEndCallback = [](ImGuiInputTextCallbackData* data) -> int {
		if (s_MoveCursorToEnd) {
			data->CursorPos = data->BufTextLen;
			data->SelectionStart = data->CursorPos;
			data->SelectionEnd = data->CursorPos;
			s_MoveCursorToEnd = false;
		}
		return 0;
	};

	bool enterPressed = ImGui::InputText("##TypeInput", s_TextBuffer, sizeof(s_TextBuffer),
										 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
										 moveCursorToEndCallback);

	float minV = std::min(minValue, maxValue);
	float maxV = std::max(minValue, maxValue);

	if (enterPressed) {
		float oldValue = value;
		value = std::clamp((float)atof(s_TextBuffer), minV, maxV);
		CommitEditImmediate(oldValue);
		changed = true;
		s_TypingID = 0;
	} else if (ImGui::IsItemDeactivated()) {
		s_MoveCursorToEnd = false;
		if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			s_TypingID = 0;
		} else {
			float oldValue = value;
			value = std::clamp((float)atof(s_TextBuffer), minV, maxV);
			CommitEditImmediate(oldValue);
			changed = true;
			s_TypingID = 0;
		}
	}

	ImGui::PopItemWidth();
	changed |= HandleCommonInteractions();

	if (yOffset != 0.0f)
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + yOffset); // balance

	return changed;
}

float ContinuousParameter::GetSafeMouseDeltaY() const {
	float deltaY = ImGui::GetIO().MouseDelta.y;
	if (deltaY != 0.0f) {
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		float teleportDistance = viewport->Size.y - 40.0f;
		float maxMovement = std::max(teleportDistance * 0.5f, 50.0f);

		// filter out massive delta spikes caused by wrapping teleports
		if (std::abs(deltaY) < maxMovement) {
			return deltaY;
		}
	}
	return 0.0f;
}

void ContinuousParameter::HandleInfiniteDrag() {
	ImVec2 mousePos = ImGui::GetIO().MousePos;
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	float wrapMargin = 20.0f;
	bool wrapped = false;

	if (viewport->Size.y > wrapMargin * 3.0f) {
		if (mousePos.y <= viewport->Pos.y + wrapMargin) {
			mousePos.y = viewport->Pos.y + viewport->Size.y - wrapMargin - 1.0f;
			wrapped = true;
		} else if (mousePos.y >= viewport->Pos.y + viewport->Size.y - wrapMargin) {
			mousePos.y = viewport->Pos.y + wrapMargin + 1.0f;
			wrapped = true;
		}
	}

	if (viewport->Size.x > wrapMargin * 3.0f) {
		if (mousePos.x <= viewport->Pos.x + wrapMargin) {
			mousePos.x = viewport->Pos.x + viewport->Size.x - wrapMargin - 1.0f;
			wrapped = true;
		} else if (mousePos.x >= viewport->Pos.x + viewport->Size.x - wrapMargin) {
			mousePos.x = viewport->Pos.x + wrapMargin + 1.0f;
			wrapped = true;
		}
	}

	if (wrapped) {
		ImGui::GetIO().WantSetMousePos = true;
		ImGui::GetIO().MousePos = mousePos;
	}
}

void ContinuousParameter::RestoreMousePosition() {
	ImGui::GetIO().WantSetMousePos = true;
	ImGui::GetIO().MousePos = ImGui::GetIO().MouseClickedPos[0];
}
