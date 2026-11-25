#include "PrecompHeader.h"
#include "Widgets.h"
#include "Theme.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

	// the share of a button's shorter side its glyph fills: big enough to read at a
	// glance, small enough to leave the box a margin on every side
	constexpr float kIconFill = 0.72f;

	// what a label shows: everything before an imgui "##" id suffix
	const char* VisibleEnd(const char* label) {
		const char* hidden = strstr(label, "##");
		return hidden ? hidden : label + strlen(label);
	}

	// the tone a button of either frame sits in, or 0 for none
	ImU32 RestingFill(Widgets::Frame frame, bool hovered, bool held) {
		const Theme& th = Theme::Instance();
		if (frame == Widgets::Frame::Boxed)
			return held ? th.buttonActive : (hovered ? th.buttonHover : th.button);
		// unboxed: only a wash under the pointer
		return held ? th.ghostPress : (hovered ? th.ghostHover : 0);
	}

	// an engaged button's fill, lifted a little under the pointer so it still answers it
	ImU32 EngagedFill(ImU32 activeColor, bool hovered, bool held) {
		const Theme& th = Theme::Instance();
		if (held)
			return Theme::Lerp(activeColor, th.bgDeepest, 0.12f);
		return hovered ? Theme::Lerp(activeColor, th.text, 0.14f) : activeColor;
	}

	void PaintBox(const ImVec2& min, const ImVec2& max, ImU32 fill) {
		if (fill == 0)
			return;
		ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(fill), ImGui::GetStyle().FrameRounding);
	}

	ImVec2 Center(const ImVec2& min, const ImVec2& max) {
		return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
	}

} // namespace

namespace Widgets {

	bool IconButton(const char* id, Icon icon, const ImVec2& size, Frame frame, const char* tooltip) {
		const Theme& th = Theme::Instance();
		const bool pressed = ImGui::InvisibleButton(id, size);
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();

		PaintBox(min, max, RestingFill(frame, hovered, held));

		// a boxed or plain glyph reads in the full text tone; a ghost one stays muted
		// until the pointer reaches it
		const ImU32 ink = (frame != Frame::Ghost || hovered || held) ? th.text : th.textMuted;
		Icons::Draw(ImGui::GetWindowDrawList(), icon, Center(min, max), std::min(size.x, size.y) * kIconFill,
					ImGui::GetColorU32(ink));

		if (tooltip && hovered)
			ImGui::SetTooltip("%s", tooltip);
		return pressed;
	}

	bool IconToggle(const char* id, Icon icon, bool active, const ImVec2& size, ImU32 activeColor, Frame frame, const char* tooltip) {
		const Theme& th = Theme::Instance();
		const bool pressed = ImGui::InvisibleButton(id, size);
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();

		ImU32 ink;
		if (active) {
			const ImU32 fill = EngagedFill(activeColor, hovered, held);
			PaintBox(min, max, fill);
			ink = Theme::ContrastingText(fill, th.text, th.textOnAccent);
		} else {
			// at rest a boxed or plain toggle shows its glyph in the full text tone, the
			// way a ghost one does only under the pointer
			PaintBox(min, max, RestingFill(frame, hovered, held));
			ink = (frame != Frame::Ghost || hovered || held) ? th.text : th.textMuted;
		}
		Icons::Draw(ImGui::GetWindowDrawList(), icon, Center(min, max), std::min(size.x, size.y) * kIconFill,
					ImGui::GetColorU32(ink));

		if (tooltip && hovered)
			ImGui::SetTooltip("%s", tooltip);
		return pressed;
	}

	bool TextToggle(const char* label, bool active, const ImVec2& size, ImU32 activeColor, const char* tooltip) {
		const Theme& th = Theme::Instance();
		const ImGuiStyle& style = ImGui::GetStyle();
		const char* end = VisibleEnd(label);
		const ImVec2 textSize = ImGui::CalcTextSize(label, end);
		const ImVec2 frameSize(size.x > 0.0f ? size.x : textSize.x + style.FramePadding.x * 2.0f,
							   size.y > 0.0f ? size.y : ImGui::GetFrameHeight());

		const bool pressed = ImGui::InvisibleButton(label, frameSize);
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();

		ImU32 ink;
		if (active) {
			const ImU32 fill = EngagedFill(activeColor != 0 ? activeColor : th.accent, hovered, held);
			PaintBox(min, max, fill);
			ink = Theme::ContrastingText(fill, th.text, th.textOnAccent);
		} else {
			PaintBox(min, max, RestingFill(Frame::Boxed, hovered, held));
			ink = th.text;
		}

		// centered in the box, and clipped to it when the caller gave less room than the
		// caption wants
		const ImVec2 textPos(std::max(min.x + (frameSize.x - textSize.x) * 0.5f, min.x + style.FramePadding.x * 0.5f),
							 min.y + (frameSize.y - textSize.y) * 0.5f);
		ImGui::GetWindowDrawList()->PushClipRect(min, max, true);
		ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(ink), label, end);
		ImGui::GetWindowDrawList()->PopClipRect();

		if (tooltip && hovered)
			ImGui::SetTooltip("%s", tooltip);
		return pressed;
	}

	bool IconTextButton(const char* label, Icon icon, const ImVec2& size, const char* tooltip) {
		const Theme& th = Theme::Instance();
		const ImGuiStyle& style = ImGui::GetStyle();
		const char* end = VisibleEnd(label);
		const ImVec2 textSize = ImGui::CalcTextSize(label, end);
		const float glyph = ImGui::GetTextLineHeight() * 0.7f;
		const float contentWidth = glyph + style.ItemInnerSpacing.x + textSize.x;
		const ImVec2 frameSize(size.x > 0.0f ? size.x : contentWidth + style.FramePadding.x * 2.0f,
							   size.y > 0.0f ? size.y : ImGui::GetFrameHeight());

		const bool pressed = ImGui::InvisibleButton(label, frameSize);
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		PaintBox(min, max, RestingFill(Frame::Boxed, hovered, held));

		const float left = min.x + std::max((frameSize.x - contentWidth) * 0.5f, style.FramePadding.x * 0.5f);
		const float middle = (min.y + max.y) * 0.5f;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		Icons::Draw(drawList, icon, ImVec2(left + glyph * 0.5f, middle), glyph, ImGui::GetColorU32(th.text));
		drawList->PushClipRect(min, max, true);
		drawList->AddText(ImVec2(left + glyph + style.ItemInnerSpacing.x, middle - textSize.y * 0.5f),
						  ImGui::GetColorU32(th.text), label, end);
		drawList->PopClipRect();

		if (tooltip && hovered)
			ImGui::SetTooltip("%s", tooltip);
		return pressed;
	}

	bool SearchField(const char* id, char* buffer, size_t bufferSize, const char* hint, ImGuiInputTextFlags flags) {
		const Theme& th = Theme::Instance();
		const ImGuiStyle& style = ImGui::GetStyle();
		const float glyph = ImGui::GetTextLineHeight() * 0.72f;

		// the text starts past the magnifier, so the padding grows by the glyph's width
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
							ImVec2(style.FramePadding.x + glyph + style.ItemInnerSpacing.x, style.FramePadding.y));
		const bool changed = ImGui::InputTextWithHint(id, hint, buffer, bufferSize, flags);
		ImGui::PopStyleVar();

		const bool focused = ImGui::IsItemActive();
		Icons::Draw(ImGui::GetWindowDrawList(), Icon::Search,
					ImVec2(pos.x + style.FramePadding.x + glyph * 0.5f, pos.y + ImGui::GetFrameHeight() * 0.5f), glyph,
					ImGui::GetColorU32(focused ? th.text : th.textDim));
		return changed;
	}

	void Caption(const char* label, Icon icon) {
		const Theme& th = Theme::Instance();
		const ImGuiStyle& style = ImGui::GetStyle();

		// the row is as tall as the body face's controls, so a caption lines up with a
		// button beside it; only the caption itself is set small
		const float rowHeight = ImGui::GetFrameHeight();
		PushSmallStrongFont();
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float glyph = ImGui::GetTextLineHeight() * 0.95f;
		float x = pos.x;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		if (icon != Icon::Count) {
			Icons::Draw(drawList, icon, ImVec2(x + glyph * 0.5f, pos.y + rowHeight * 0.5f), glyph, ImGui::GetColorU32(th.textMuted));
			x += glyph + style.ItemInnerSpacing.x;
		}
		const ImVec2 textSize = ImGui::CalcTextSize(label);
		drawList->AddText(ImVec2(x, pos.y + (rowHeight - textSize.y) * 0.5f), ImGui::GetColorU32(th.textMuted), label);
		ImGui::Dummy(ImVec2(x - pos.x + textSize.x, rowHeight));
		ImGui::PopFont();
	}

	bool ListRow(const char* id, Icon icon, const char* label, bool selected, ImU32 labelColor, ImU32 iconColor,
				 const char* tag, ImGuiSelectableFlags flags) {
		// the selectable is submitted with no visible label - its highlight spans the row
		// either way - and the face is painted onto the line it was given
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const bool pressed = ImGui::Selectable(id, selected, flags);
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		ListRowFace(origin, ImGui::GetItemRectMax().x - spacing * 0.5f, icon, label, labelColor, iconColor, tag);
		return pressed;
	}

	void ListRowFace(const ImVec2& origin, float right, Icon icon, const char* label, ImU32 labelColor, ImU32 iconColor,
					 const char* tag) {
		const Theme& th = Theme::Instance();
		const ImGuiStyle& style = ImGui::GetStyle();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const float lineHeight = ImGui::GetTextLineHeight();
		const float glyph = std::floor(lineHeight * 0.78f);
		const float middle = origin.y + lineHeight * 0.5f;

		Icons::Draw(drawList, icon, ImVec2(origin.x + glyph * 0.5f, middle), glyph, ImGui::GetColorU32(iconColor != 0 ? iconColor : th.textMuted));

		// the tag claims its room first, and the name is cut off short of it
		float nameRight = right;
		if (tag && tag[0]) {
			PushSmallFont();
			const ImVec2 tagSize = ImGui::CalcTextSize(tag);
			nameRight = right - tagSize.x - style.ItemInnerSpacing.x;
			drawList->AddText(ImVec2(right - tagSize.x, middle - tagSize.y * 0.5f), ImGui::GetColorU32(th.textDim), tag);
			ImGui::PopFont();
		}
		const float nameX = origin.x + glyph + style.ItemInnerSpacing.x + 1.0f;
		drawList->PushClipRect(ImVec2(nameX, origin.y), ImVec2(std::max(nameX, nameRight), origin.y + lineHeight), true);
		drawList->AddText(ImVec2(nameX, origin.y), ImGui::GetColorU32(labelColor != 0 ? labelColor : th.text), label);
		drawList->PopClipRect();
	}

	bool BeginCombo(const char* id, const char* preview, ImGuiComboFlags flags) {
		// imgui paints the arrow's box in the button tones; lending it the field's own
		// tones leaves just the arrow. popped straight after, so nothing inside the open
		// list inherits them
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
		const bool open = ImGui::BeginCombo(id, preview, flags);
		ImGui::PopStyleColor(2);
		return open;
	}

	bool BeginFloatingWindow(const char* name, bool* open, ImGuiWindowFlags flags) {
		// the title bar is drawn inside Begin, so the face only has to be in place for
		// that call; the window's contents go back to the regular one
		PushStrongFont();
		const bool visible = ImGui::Begin(name, open, flags);
		ImGui::PopFont();
		return visible;
	}

	void PushStrongFont() {
		ImGui::PushFont(Theme::Instance().fontStrong, 0.0f);
	}

	void PushSmallFont() {
		ImGui::PushFont(nullptr, Theme::Instance().fontSizeSmall);
	}

	void PushSmallStrongFont() {
		ImGui::PushFont(Theme::Instance().fontStrong, Theme::Instance().fontSizeSmall);
	}

} // namespace Widgets
