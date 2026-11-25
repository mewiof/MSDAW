#include "PrecompHeader.h"
#include "Theme.h"
#include <cmath>
#include <cstdio>

Theme& Theme::Instance() {
	static Theme instance;
	return instance;
}

// the concrete default palette: exact neutral grays with a single orange accent. this
// is the ONE place the numbers live
Theme::Theme() {
	// every surface is a pure gray - equal red, green and blue - so nothing on screen
	// carries a tint of its own and the few colors that mean something (the accent, a
	// track's color, a meter going red) are the only hues there are
	auto Gray = [](int level) { return IM_COL32(level, level, level, 255); };

	// surfaces, in the reference product's tonal order rather than its exact values: the
	// chrome and the bars are darker than the panels they frame, a device's title strip
	// is lighter than its body, and graphs sit in near-black wells. stepped by roughly
	// even perceptual distance, since near black a few levels apart already separate
	bgDeepest = Gray(24);			// wells, plot interiors, bypassed device
	bgChrome = Gray(30);			// menu bar, the gaps between windows
	inputBg = Gray(36);				// recessed control backgrounds
	bgWindow = Gray(42);			// device strip ground, arrangement canvas
	bgRackInner = Gray(48);			// recessed ground inside a rack, so its panels read as inset
	bgLaneGroup = Gray(47);			// group lane, recessed: nothing lives here
	bgHeader = Gray(50);			// transport, tab strip, ruler, section heads
	bgLane = Gray(55);				// timeline lane, a step above the canvas under it
	bgPanel = Gray(58);				// library, clip view, track rows, floating windows
	bgDeviceEffect = Gray(60);		// effect device body, a raised card on the strip
	bgLaneSelected = Gray(64);		// selected track lane, one visible step up
	bgDeviceInstrument = Gray(64);	// instrument body, heavier than an effect
	bgPanelAlt = Gray(66);			// raised surfaces: popups, group rows, the master row
	bgDeviceRack = Gray(68);		// a rack shell, a step above the devices it holds
	deviceTitle = Gray(74);			// a device's name strip, lighter than its body
	bgHover = Gray(78);				// hovered frame/row
	bgActive = Gray(96);			// selected row (neutral, never the accent)
	deviceTitleSelected = Gray(98); // the selected device's name strip
	bgOverlay = IM_COL32(20, 20, 20, 236);

	// controls. a button rests a clear step above the highest surface it lands on (a
	// popup, a group row or the master row) and below a selected row, and lightens
	// again under the pointer. an unboxed icon button has no tone of its own at rest,
	// only a wash of the text color once the pointer finds it
	button = Gray(84);
	buttonHover = Gray(98);
	buttonActive = Gray(112);
	ghostHover = IM_COL32(255, 255, 255, 22);
	ghostPress = IM_COL32(255, 255, 255, 40);
	// the level behind a value box's number: plainly a fill against the recessed box,
	// and still dark enough to read white digits on. dragged, it takes the accent's hue
	// at a depth the digits still read on
	valueFill = Gray(88);
	valueFillActive = IM_COL32(128, 84, 28, 255);
	// a dial is a dark groove with a light value arc, so it reads at rest without
	// borrowing the accent the touched control is marked with
	knobTrack = Gray(32);
	knobValue = Gray(196);

	muteActive = IM_COL32(232, 150, 52, 255);
	soloActive = IM_COL32(94, 148, 212, 255);
	automationActive = IM_COL32(220, 92, 78, 255);

	// lines & borders. structure comes from dark grooves - a control sunk into its panel,
	// a panel set into the chrome - and the light rule is kept for the few lines that must
	// read over a dark ground and a mid one alike
	border = Gray(28);
	borderStrong = Gray(104);
	divider = Gray(22); // near-black grout between rows/sections

	// grid - white at low opacity, so the same three lines lighten whatever lane, graph or
	// piano-roll row they cross by the same visible step. present as structure, never
	// louder than what is drawn on top of it. sub < beat < bar
	gridSub = IM_COL32(255, 255, 255, 9);
	gridBeat = IM_COL32(255, 255, 255, 18);
	gridBar = IM_COL32(255, 255, 255, 34);

	// text. the primary tone stops short of white, which glares at this size on gray
	text = Gray(224);
	textMuted = Gray(156);
	textDim = Gray(106);
	textOnAccent = Gray(20);

	// accent - orange, used sparingly
	accent = IM_COL32(232, 150, 52, 255);
	accentHover = IM_COL32(246, 172, 80, 255);
	accentActive = IM_COL32(204, 126, 36, 255);
	accentMuted = IM_COL32(138, 94, 42, 255);
	accentTranslucent = IM_COL32(232, 150, 52, 44);

	// status & semantic. the playhead is the brightest line on the canvas rather than a
	// color: it is the one thing that moves, so it needs no hue to be found
	playhead = Gray(236);
	recording = IM_COL32(226, 70, 60, 255);
	marker = IM_COL32(232, 150, 52, 255);
	selectionFill = IM_COL32(255, 255, 255, 20);
	selectionStroke = IM_COL32(220, 220, 220, 210);
	dropLine = IM_COL32(232, 150, 52, 255);
	ghost = IM_COL32(190, 190, 190, 190);
	success = IM_COL32(104, 190, 112, 255);
	danger = IM_COL32(222, 88, 76, 255);
	graphCurve = IM_COL32(236, 166, 72, 255);
	graphCurveCool = IM_COL32(124, 170, 210, 255);
	spectrumFill = IM_COL32(160, 160, 160, 44);
	spectrumEdge = IM_COL32(176, 176, 176, 110);
	spectrumFillAlt = IM_COL32(124, 170, 210, 40);
	spectrumEdgeAlt = IM_COL32(136, 180, 218, 120);

	// the spectrogram ramp climbs out of the panel background rather than out of
	// black, so a quiet passage reads as empty instead of as a hole in the panel
	spectrogramFloor = Gray(20);
	spectrogramMid = IM_COL32(64, 86, 120, 255);
	spectrogramPeak = IM_COL32(240, 196, 120, 255);

	// meters (peak level ramp)
	meterBg = Gray(20);
	meterLow = IM_COL32(92, 186, 104, 255);
	meterMid = IM_COL32(214, 192, 72, 255);
	meterHigh = IM_COL32(222, 84, 70, 255);

	// clips & waveforms. a clip's edge is a dark line, so two clips butted against each
	// other still read as two
	clipText = Gray(236);
	clipTextDim = Gray(150);
	clipBorder = IM_COL32(0, 0, 0, 120);
	clipDisabled = Gray(86);
	clipLinked = IM_COL32(120, 200, 230, 235);
	waveform = IM_COL32(230, 230, 230, 210);
	waveBgMid = IM_COL32(30, 70, 30, 180);
	clipBodyTint = 0.36f;
	clipBodyTintSelected = 0.56f;

	// piano roll
	keyWhite = Gray(212);
	keyBlack = Gray(38);
	keyText = Gray(40);
	keyPressed = IM_COL32(232, 150, 52, 255);
	rowWhite = Gray(58);
	rowBlack = Gray(48);
	noteFill = IM_COL32(226, 156, 72, 235);
	noteFillSelected = IM_COL32(246, 206, 140, 255);
	noteBorder = IM_COL32(0, 0, 0, 110);
	noteBorderSelected = IM_COL32(255, 255, 255, 255);
	veloStem = IM_COL32(176, 116, 64, 170);
	veloStemSelected = IM_COL32(250, 200, 120, 210);
	veloDot = IM_COL32(232, 150, 80, 255);
	veloDotSelected = IM_COL32(255, 214, 140, 255);

	// automation
	automationLine = IM_COL32(226, 104, 84, 220);
	automationPoint = IM_COL32(236, 190, 96, 255);
	automationPointSelected = IM_COL32(255, 255, 255, 255);
	automationGhost = IM_COL32(220, 220, 220, 46);
}

// ================================================================
// TYPOGRAPHY
// ================================================================

void Theme::LoadFonts(float scale) {
	ImGuiIO& io = ImGui::GetIO();

	// 13 px is the size that puts the face's capitals at the height the reference
	// product sets its own at, at every display scale
	fontSize = 13.0f * scale;
	fontSizeSmall = 11.0f * scale;

	// the system face, in its regular and semibold cuts. a machine without them falls
	// back to imgui's own font for both, which keeps every label legible if plainer
	auto load = [&](const char* path) -> ImFont* {
		FILE* file = fopen(path, "rb");
		if (!file)
			return nullptr;
		fclose(file);
		return io.Fonts->AddFontFromFileTTF(path, fontSize);
	};

	// the first font added is the one every window starts out in
	fontRegular = load("C:\\Windows\\Fonts\\segoeui.ttf");
	if (!fontRegular)
		fontRegular = io.Fonts->AddFontDefault();
	fontStrong = load("C:\\Windows\\Fonts\\seguisb.ttf");
	if (!fontStrong)
		fontStrong = fontRegular;
}

// ================================================================
// IMGUI STYLE
// ================================================================

void Theme::ApplyImGuiStyle(float scale) const {
	// start from the stock dark theme so any color we forget stays sensible,
	// then override with our palette
	ImGui::StyleColorsDark();

	ImGuiStyle& style = ImGui::GetStyle();

	// metrics, unscaled - ScaleAllSizes below takes them to the display scale. the
	// controls are compact: a frame is the text plus a few pixels, the way a dense
	// desktop tool lays them out, rather than a touch target
	style.WindowPadding = ImVec2(8.0f, 6.0f);
	style.FramePadding = ImVec2(6.0f, 3.0f);
	style.ItemSpacing = ImVec2(6.0f, 4.0f);
	style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
	style.CellPadding = ImVec2(4.0f, 2.0f);
	style.IndentSpacing = 14.0f;
	style.ScrollbarSize = 10.0f;
	style.GrabMinSize = 10.0f;

	// square windows: the panels are docked edge-to-edge, and rounded corners left
	// wedge-shaped gaps of the app background where neighboring panels meet. controls
	// keep the barest rounding, enough to soften an edge without reading as a pill
	style.WindowRounding = 0.0f;
	style.ChildRounding = 0.0f;
	style.FrameRounding = 2.0f;
	style.GrabRounding = 2.0f;
	style.ScrollbarRounding = 2.0f;
	style.TabRounding = 2.0f;
	style.PopupRounding = 3.0f;

	style.ScaleAllSizes(scale);

	// border widths stay one pixel at every scale - a hairline is a hairline
	style.WindowBorderSize = 1.0f;
	// NOTE: imgui pads a child window only when it is drawn with a border or asks for
	// padding outright. with no child borders, ImGuiChildFlags_Borders alone leaves the
	// content flush against the child's edges - a pane that wants an inset says so with
	// ImGuiChildFlags_AlwaysUseWindowPadding
	style.ChildBorderSize = 0.0f;
	style.PopupBorderSize = 1.0f;
	style.FrameBorderSize = 0.0f;
	style.TabBorderSize = 0.0f;
	style.TabBarBorderSize = 1.0f;
	style.TabBarOverlineSize = 2.0f;
	style.SeparatorTextBorderSize = 1.0f;

	// local shorthands: V unpacks a packed color into the ImVec4 the style array
	// wants; T is a fully transparent slot
	auto V = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
	const ImVec4 T(0.0f, 0.0f, 0.0f, 0.0f);
	ImVec4* c = style.Colors;

	c[ImGuiCol_Text] = V(text);
	c[ImGuiCol_TextDisabled] = V(textDim);
	// a window that does not say otherwise is a panel. the dark grounds - the device
	// strip and the arrangement canvas - ask for theirs when they open
	c[ImGuiCol_WindowBg] = V(bgPanel);
	c[ImGuiCol_ChildBg] = T; // panels paint their own backgrounds
	c[ImGuiCol_PopupBg] = V(bgPanelAlt);
	// a window's edge is the dark grout between it and its neighbor
	c[ImGuiCol_Border] = V(divider);
	c[ImGuiCol_BorderShadow] = T;

	c[ImGuiCol_FrameBg] = V(inputBg); // recessed, so a control never blends into its panel
	c[ImGuiCol_FrameBgHovered] = V(Lerp(inputBg, bgHover, 0.35f));
	c[ImGuiCol_FrameBgActive] = V(Lerp(inputBg, bgActive, 0.4f)); // neutral on purpose (reserved accent)

	c[ImGuiCol_TitleBg] = V(bgChrome);
	c[ImGuiCol_TitleBgActive] = V(bgHeader);
	c[ImGuiCol_TitleBgCollapsed] = V(bgChrome);
	c[ImGuiCol_MenuBarBg] = V(bgChrome);

	c[ImGuiCol_ScrollbarBg] = V(WithAlpha(bgDeepest, 70));
	c[ImGuiCol_ScrollbarGrab] = V(button);
	c[ImGuiCol_ScrollbarGrabHovered] = V(buttonHover);
	c[ImGuiCol_ScrollbarGrabActive] = V(buttonActive);

	// active/focus signals - the only places the accent is allowed
	c[ImGuiCol_CheckMark] = V(accent);
	c[ImGuiCol_SliderGrab] = V(textMuted);	  // neutral at rest
	c[ImGuiCol_SliderGrabActive] = V(accent); // orange only while dragging

	c[ImGuiCol_Button] = V(button);
	c[ImGuiCol_ButtonHovered] = V(buttonHover);
	c[ImGuiCol_ButtonActive] = V(buttonActive); // neutral on purpose

	// Header* drive Selectable/TreeNode/MenuItem. a picked row lifts to the selected-row
	// tone, the same one a selected track takes, and a hovered one a step under it - the
	// accent would turn every list and menu into a strip of orange
	c[ImGuiCol_Header] = V(bgActive);
	c[ImGuiCol_HeaderHovered] = V(bgHover);
	c[ImGuiCol_HeaderActive] = V(bgActive);

	c[ImGuiCol_Separator] = V(border);
	c[ImGuiCol_SeparatorHovered] = V(borderStrong);
	c[ImGuiCol_SeparatorActive] = V(accentMuted);

	c[ImGuiCol_ResizeGrip] = T;
	c[ImGuiCol_ResizeGripHovered] = V(WithAlpha(accent, 130));
	c[ImGuiCol_ResizeGripActive] = V(WithAlpha(accent, 200));

	c[ImGuiCol_InputTextCursor] = V(text);

	// a tab at rest is only its caption; the open one is lifted a step and carries the
	// orange line, the way the bottom panel's tabs do. it keeps that line when its window
	// loses focus - which tab is open does not stop mattering when the pointer leaves
	c[ImGuiCol_Tab] = T;
	c[ImGuiCol_TabHovered] = V(bgHover);
	c[ImGuiCol_TabSelected] = V(bgPanelAlt);
	c[ImGuiCol_TabSelectedOverline] = V(accent);
	c[ImGuiCol_TabDimmed] = T;
	c[ImGuiCol_TabDimmedSelected] = V(bgPanelAlt);
	c[ImGuiCol_TabDimmedSelectedOverline] = V(accent);

	c[ImGuiCol_PlotLines] = V(textMuted);
	c[ImGuiCol_PlotLinesHovered] = V(accent);
	c[ImGuiCol_PlotHistogram] = V(knobValue); // a dial's value arc, unless its panel colors it
	c[ImGuiCol_PlotHistogramHovered] = V(accent);

	c[ImGuiCol_TableHeaderBg] = V(bgHeader);
	c[ImGuiCol_TableBorderStrong] = V(border);
	c[ImGuiCol_TableBorderLight] = V(divider);
	c[ImGuiCol_TableRowBg] = T;
	c[ImGuiCol_TableRowBgAlt] = V(WithAlpha(text, 8));

	c[ImGuiCol_TextLink] = V(accent);
	c[ImGuiCol_TextSelectedBg] = V(accentTranslucent);
	c[ImGuiCol_TreeLines] = V(border);
	c[ImGuiCol_DragDropTarget] = V(accent);
	c[ImGuiCol_DragDropTargetBg] = V(accentTranslucent);
	c[ImGuiCol_UnsavedMarker] = V(textMuted);

	c[ImGuiCol_NavCursor] = V(accent);
	c[ImGuiCol_NavWindowingHighlight] = V(WithAlpha(accent, 180));
	c[ImGuiCol_NavWindowingDimBg] = V(WithAlpha(bgDeepest, 150));
	c[ImGuiCol_ModalWindowDimBg] = V(WithAlpha(bgDeepest, 150));
}

// ================================================================
// COLOR HELPERS
// ================================================================

ImU32 Theme::WithAlpha(ImU32 color, int alpha) {
	alpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
	return (color & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
}

ImU32 Theme::Lerp(ImU32 a, ImU32 b, float t) {
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	auto ch = [](ImU32 col, int shift) { return (int)((col >> shift) & 0xFF); };
	auto mix = [t](int lo, int hi) { return (int)(lo + (hi - lo) * t + 0.5f); };
	int r = mix(ch(a, IM_COL32_R_SHIFT), ch(b, IM_COL32_R_SHIFT));
	int g = mix(ch(a, IM_COL32_G_SHIFT), ch(b, IM_COL32_G_SHIFT));
	int bl = mix(ch(a, IM_COL32_B_SHIFT), ch(b, IM_COL32_B_SHIFT));
	int al = mix(ch(a, IM_COL32_A_SHIFT), ch(b, IM_COL32_A_SHIFT));
	return IM_COL32(r, g, bl, al);
}

// relative luminance by the WCAG definition: each sRGB channel back to linear light,
// weighted by how bright the eye takes it to be. 0 is black, 1 is white
static float RelativeLuminance(ImU32 color) {
	auto linear = [color](int shift) {
		const float c = (float)((color >> shift) & 0xFF) / 255.0f;
		return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
	};
	return 0.2126f * linear(IM_COL32_R_SHIFT) + 0.7152f * linear(IM_COL32_G_SHIFT) + 0.0722f * linear(IM_COL32_B_SHIFT);
}

float Theme::ContrastRatio(ImU32 a, ImU32 b) {
	const float la = RelativeLuminance(a);
	const float lb = RelativeLuminance(b);
	return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

ImU32 Theme::ContrastingText(ImU32 fill, ImU32 light, ImU32 dark) {
	return ContrastRatio(fill, dark) > ContrastRatio(fill, light) ? dark : light;
}

ImU32 Theme::HeatColor(float t) const {
	if (t < 0.5f)
		return Lerp(meterLow, meterMid, t / 0.5f);
	return Lerp(meterMid, meterHigh, (t - 0.5f) / 0.5f);
}

ImU32 Theme::MeterColor(float norm) const {
	// green until it gets hot near the top, then knees toward red
	if (norm < 0.75f)
		return Lerp(meterLow, meterMid, norm / 0.75f);
	return Lerp(meterMid, meterHigh, (norm - 0.75f) / 0.25f);
}

ImU32 Theme::SpectrogramColor(float t) const {
	if (t < 0.5f)
		return Lerp(spectrogramFloor, spectrogramMid, t / 0.5f);
	return Lerp(spectrogramMid, spectrogramPeak, (t - 0.5f) / 0.5f);
}

ImU32 Theme::TrackColor(int index) const {
	// muted jewel tones picked to sit calmly on the gray without clashing with the
	// orange accent - the arrangement's only real color comes from these
	static const ImU32 palette[] = {
		IM_COL32(198, 120, 86, 255),  // terracotta
		IM_COL32(210, 168, 84, 255),  // ochre
		IM_COL32(150, 176, 100, 255), // olive
		IM_COL32(96, 176, 150, 255),  // teal
		IM_COL32(98, 154, 200, 255),  // steel blue
		IM_COL32(140, 138, 208, 255), // periwinkle
		IM_COL32(180, 128, 196, 255), // mauve
		IM_COL32(206, 118, 156, 255), // rose
		IM_COL32(170, 170, 178, 255), // slate
		IM_COL32(200, 146, 110, 255), // sand
		IM_COL32(120, 184, 176, 255), // aqua
		IM_COL32(196, 176, 120, 255), // wheat
	};
	const int n = (int)(sizeof(palette) / sizeof(palette[0]));
	int i = ((index % n) + n) % n; // wrap, tolerating negative indices
	return palette[i];
}
