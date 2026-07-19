#include "PrecompHeader.h"
#include "Theme.h"

Theme& Theme::Instance() {
	static Theme instance;
	return instance;
}

// the concrete default palette. neutral dark gray (no blue tint), Ableton-like,
// with a single amber/gold accent. this is the ONE place the numbers live
Theme::Theme() {
	// surfaces, darkest -> lightest. stepped by roughly even *perceptual* (CIE L*)
	// distance rather than even rgb: near black a few rgb apart already separates,
	// higher up you need a wider gap for the same visible jump. the old ladder was
	// even in rgb, so header/deviceEffect/panel sat ~1.7 L* apart and melted into
	// one gray wash. the canvas stays deep while panels read as raised cards on it
	bgDeepest = IM_COL32(15, 15, 17, 255);			// wells, plot interiors, bypassed device
	inputBg = IM_COL32(24, 24, 27, 255);			// recessed control backgrounds
	bgWindow = IM_COL32(32, 32, 36, 255);			// main window + timeline canvas
	bgHeader = IM_COL32(40, 40, 45, 255);			// sticky header strips (a step under panel)
	bgDeviceEffect = IM_COL32(46, 46, 51, 255);		// effect device slot, a raised card on the canvas
	bgPanel = IM_COL32(52, 52, 58, 255);			// track rows, ruler, ordinary panels
	bgDeviceInstrument = IM_COL32(62, 62, 69, 255); // instrument slot, heavier than an effect
	bgPanelAlt = IM_COL32(68, 68, 75, 255);			// raised buttons, popups, group rows
	bgHover = IM_COL32(84, 84, 92, 255);			// hovered frame/row/button
	bgActive = IM_COL32(100, 100, 109, 255);		// selected row (neutral, never the accent)
	bgOverlay = IM_COL32(18, 18, 22, 236);			// translucent box floating over the canvas

	// lines & borders - kept a hair brighter than the surfaces they sit on so
	// delineation comes from crisp 1px edges, not from cranking fill contrast
	border = IM_COL32(78, 78, 86, 255);
	borderStrong = IM_COL32(104, 104, 114, 255);
	divider = IM_COL32(14, 14, 16, 255); // near-black grout between rows/sections

	// grid - the old bar line landed ~20 L* over the canvas and read as a hard
	// fence competing with the clips. these sit only a few L* above the background:
	// present as structure, never louder than what is drawn on top. sub < beat < bar
	gridSub = IM_COL32(105, 105, 116, 28);
	gridBeat = IM_COL32(114, 114, 126, 55);
	gridBar = IM_COL32(122, 122, 136, 78);

	// text
	text = IM_COL32(230, 230, 230, 255);
	textMuted = IM_COL32(168, 168, 172, 255);
	textDim = IM_COL32(110, 110, 116, 255);
	textOnAccent = IM_COL32(20, 16, 0, 255);

	// accent - amber/gold, used sparingly
	accent = IM_COL32(224, 162, 48, 255);
	accentHover = IM_COL32(240, 184, 72, 255);
	accentActive = IM_COL32(200, 138, 32, 255);
	accentMuted = IM_COL32(138, 106, 30, 255);
	accentTranslucent = IM_COL32(224, 162, 48, 40);

	// status & semantic
	playhead = IM_COL32(240, 70, 60, 255);
	recording = IM_COL32(235, 64, 52, 255);
	marker = IM_COL32(96, 196, 112, 255);
	selectionFill = IM_COL32(224, 162, 48, 32);
	selectionStroke = IM_COL32(240, 200, 110, 220);
	dropLine = IM_COL32(224, 162, 48, 255);
	ghost = IM_COL32(180, 180, 190, 200);
	success = IM_COL32(96, 196, 112, 255);
	danger = IM_COL32(224, 80, 72, 255);
	graphCurve = IM_COL32(235, 180, 70, 255);
	graphCurveCool = IM_COL32(96, 176, 232, 255);

	// meters (peak level ramp)
	meterBg = IM_COL32(20, 20, 22, 255);
	meterLow = IM_COL32(96, 196, 112, 255);
	meterMid = IM_COL32(224, 196, 64, 255);
	meterHigh = IM_COL32(224, 80, 72, 255);

	// clips & waveforms
	clipText = IM_COL32(240, 240, 240, 255);
	clipBorder = IM_COL32(255, 255, 255, 80);
	waveform = IM_COL32(230, 230, 235, 200);
	waveBgMono = IM_COL32(0, 0, 0, 200);
	waveBgMid = IM_COL32(30, 70, 30, 180);

	// piano roll
	keyWhite = IM_COL32(230, 230, 230, 255);
	keyBlack = IM_COL32(50, 50, 50, 255);
	keyText = IM_COL32(0, 0, 0, 255);
	keyPressed = IM_COL32(224, 162, 48, 255);
	rowWhite = IM_COL32(60, 60, 66, 255);
	rowBlack = IM_COL32(38, 38, 43, 255);
	noteFill = IM_COL32(222, 158, 74, 235);
	noteFillSelected = IM_COL32(245, 205, 130, 255);
	noteBorder = IM_COL32(255, 255, 255, 120);
	noteBorderSelected = IM_COL32(255, 255, 255, 255);
	veloStem = IM_COL32(170, 110, 70, 160);
	veloStemSelected = IM_COL32(255, 200, 120, 200);
	veloDot = IM_COL32(235, 150, 90, 255);
	veloDotSelected = IM_COL32(255, 215, 140, 255);

	// automation
	automationLine = IM_COL32(232, 116, 84, 210);
	automationPoint = IM_COL32(240, 200, 90, 255);
	automationPointSelected = IM_COL32(255, 255, 255, 255);
	automationGhost = IM_COL32(200, 200, 200, 50);
}

// ================================================================
// IMGUI STYLE
// ================================================================

void Theme::ApplyImGuiStyle(float scale) const {
	// start from the stock dark theme so any color we forget stays sensible,
	// then override with our palette
	ImGui::StyleColorsDark();

	ImGuiStyle& style = ImGui::GetStyle();
	style.ScaleAllSizes(scale);
	// square windows: the panels are docked edge-to-edge, and rounded corners left
	// wedge-shaped gaps of the app background where neighboring panels meet. popups and
	// tooltips keep their own rounding (PopupRounding below)
	style.WindowRounding = 0.0f;
	style.FrameRounding = 3.0f;
	style.GrabRounding = 2.0f;
	style.ScrollbarRounding = 3.0f;
	style.TabRounding = 3.0f;
	style.PopupRounding = 3.0f;
	style.WindowBorderSize = 1.0f;
	style.FrameBorderSize = 1.0f; // give inputs/buttons a crisp edge so they read as controls

	// local shorthands: V unpacks a packed color into the ImVec4 the style array
	// wants; T is a fully transparent slot
	auto V = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
	const ImVec4 T(0.0f, 0.0f, 0.0f, 0.0f);
	ImVec4* c = style.Colors;

	c[ImGuiCol_Text] = V(text);
	c[ImGuiCol_TextDisabled] = V(textDim);
	c[ImGuiCol_WindowBg] = V(bgWindow);
	c[ImGuiCol_ChildBg] = T; // panels paint their own backgrounds
	c[ImGuiCol_PopupBg] = V(bgPanelAlt);
	c[ImGuiCol_Border] = V(border);
	c[ImGuiCol_BorderShadow] = T;

	c[ImGuiCol_FrameBg] = V(inputBg); // recessed, so a control never blends into its panel
	c[ImGuiCol_FrameBgHovered] = V(Lerp(inputBg, bgHover, 0.45f));
	c[ImGuiCol_FrameBgActive] = V(Lerp(inputBg, bgActive, 0.55f)); // neutral on purpose (reserved accent)

	c[ImGuiCol_TitleBg] = V(bgHeader);
	c[ImGuiCol_TitleBgActive] = V(bgPanelAlt);
	c[ImGuiCol_TitleBgCollapsed] = V(bgHeader);
	c[ImGuiCol_MenuBarBg] = V(bgHeader);

	c[ImGuiCol_ScrollbarBg] = V(WithAlpha(bgDeepest, 130));
	c[ImGuiCol_ScrollbarGrab] = V(border);
	c[ImGuiCol_ScrollbarGrabHovered] = V(borderStrong);
	c[ImGuiCol_ScrollbarGrabActive] = V(textDim);

	// active/focus signals - the only places the accent is allowed
	c[ImGuiCol_CheckMark] = V(accent);
	c[ImGuiCol_SliderGrab] = V(textMuted);	  // neutral at rest
	c[ImGuiCol_SliderGrabActive] = V(accent); // amber only while dragging

	c[ImGuiCol_Button] = V(bgPanelAlt);
	c[ImGuiCol_ButtonHovered] = V(bgHover);
	c[ImGuiCol_ButtonActive] = V(bgActive); // neutral on purpose

	// Header* drive Selectable/TreeNode/MenuItem: selection gets a muted amber
	// wash, but hover stays gray so lists/menus do not flash accent constantly
	c[ImGuiCol_Header] = V(WithAlpha(accent, 90));
	c[ImGuiCol_HeaderHovered] = V(bgHover);
	c[ImGuiCol_HeaderActive] = V(WithAlpha(accent, 130));

	c[ImGuiCol_Separator] = V(border);
	c[ImGuiCol_SeparatorHovered] = V(borderStrong);
	c[ImGuiCol_SeparatorActive] = V(accentMuted);

	c[ImGuiCol_ResizeGrip] = V(WithAlpha(textDim, 60));
	c[ImGuiCol_ResizeGripHovered] = V(WithAlpha(accent, 130));
	c[ImGuiCol_ResizeGripActive] = V(WithAlpha(accent, 200));

	c[ImGuiCol_InputTextCursor] = V(text);

	c[ImGuiCol_Tab] = V(bgPanel);
	c[ImGuiCol_TabHovered] = V(bgHover);
	c[ImGuiCol_TabSelected] = V(bgPanelAlt);
	c[ImGuiCol_TabSelectedOverline] = V(accent); // thin amber line marks the active tab
	c[ImGuiCol_TabDimmed] = V(bgPanel);
	c[ImGuiCol_TabDimmedSelected] = V(bgPanelAlt);
	c[ImGuiCol_TabDimmedSelectedOverline] = V(WithAlpha(accentMuted, 150));

	c[ImGuiCol_PlotLines] = V(textMuted);
	c[ImGuiCol_PlotLinesHovered] = V(accent);
	c[ImGuiCol_PlotHistogram] = V(textMuted); // knob value arcs stay neutral gray
	c[ImGuiCol_PlotHistogramHovered] = V(accent);

	c[ImGuiCol_TableHeaderBg] = V(bgHeader);
	c[ImGuiCol_TableBorderStrong] = V(borderStrong);
	c[ImGuiCol_TableBorderLight] = V(border);
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

ImU32 Theme::TrackColor(int index) const {
	// muted jewel tones picked to sit calmly on the dark gray without clashing
	// with the amber accent
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
