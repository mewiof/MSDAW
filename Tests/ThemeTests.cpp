#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include "Theme.h"

// ================================================================
// TEXT ON A COLOR THE USER PICKED
// ================================================================

// a clip's name and notes are drawn straight onto its track's color, and that color can
// be anything from near black to near white - more so once a selected clip brightens it.
// these pin the rule that picks the tone written on it, not the palette: the higher
// contrast wins, whichever order the two tones arrive in

namespace {

	const ImU32 kBlack = IM_COL32(0, 0, 0, 255);
	const ImU32 kWhite = IM_COL32(255, 255, 255, 255);
	const ImU32 kLightText = IM_COL32(240, 240, 240, 255);
	const ImU32 kDarkText = IM_COL32(20, 16, 0, 255);

} // namespace

TEST(Theme, ContrastRatioSpansSameColorToBlackOnWhite) {
	EXPECT_FLOAT_EQ(Theme::ContrastRatio(kWhite, kWhite), 1.0f);
	EXPECT_FLOAT_EQ(Theme::ContrastRatio(kBlack, kBlack), 1.0f);
	EXPECT_NEAR(Theme::ContrastRatio(kBlack, kWhite), 21.0f, 0.01f);
}

TEST(Theme, ContrastRatioIsSymmetricAndIgnoresAlpha) {
	const ImU32 amber = IM_COL32(224, 162, 48, 255);
	const ImU32 slate = IM_COL32(52, 52, 58, 255);
	EXPECT_FLOAT_EQ(Theme::ContrastRatio(amber, slate), Theme::ContrastRatio(slate, amber));
	EXPECT_FLOAT_EQ(Theme::ContrastRatio(Theme::WithAlpha(amber, 40), slate), Theme::ContrastRatio(amber, slate));
}

TEST(Theme, ContrastingTextWritesDarkOnALightFillAndLightOnADarkOne) {
	EXPECT_EQ(Theme::ContrastingText(kWhite, kLightText, kDarkText), kDarkText);
	EXPECT_EQ(Theme::ContrastingText(IM_COL32(252, 201, 100, 255), kLightText, kDarkText), kDarkText);
	EXPECT_EQ(Theme::ContrastingText(kBlack, kLightText, kDarkText), kLightText);
	EXPECT_EQ(Theme::ContrastingText(IM_COL32(40, 40, 60, 255), kLightText, kDarkText), kLightText);
}

TEST(Theme, ContrastingTextDoesNotDependOnWhichToneComesFirst) {
	const ImU32 fills[] = {kBlack, kWhite, IM_COL32(98, 154, 200, 255), IM_COL32(198, 120, 86, 255), IM_COL32(74, 76, 82, 255)};
	for (ImU32 fill : fills)
		EXPECT_EQ(Theme::ContrastingText(fill, kLightText, kDarkText), Theme::ContrastingText(fill, kDarkText, kLightText));
}

// ================================================================
// BUTTONS
// ================================================================

// the button tone was once the same as the popups, the group rows and the master row
// it gets drawn on, so a button there was bare text until the pointer reached it. a
// button has to stand apart from every surface the app puts one on, and from its own
// hovered state. the floor is the rule - "plainly not the same tone" - not a look

TEST(Theme, ButtonsStandApartFromEverySurfaceTheySitOn) {
	const Theme& th = Theme::Instance();
	const ImU32 surfaces[] = {th.bgWindow, th.bgHeader, th.bgPanel, th.bgPanelAlt, th.bgDeviceEffect,
							  th.bgDeviceInstrument, th.bgDeviceRack, th.bgRackInner, th.bgActive};
	for (ImU32 surface : surfaces)
		EXPECT_GT(Theme::ContrastRatio(th.button, surface), 1.15f);

	EXPECT_GT(Theme::ContrastRatio(th.buttonHover, th.button), 1.1f);
	EXPECT_GT(Theme::ContrastRatio(th.buttonActive, th.button), 1.1f);
}

// ================================================================
// VALUE BOXES
// ================================================================

// a value box writes its number in the ordinary text tone straight over its level fill,
// so the fill has to leave that number readable - the light gray it used before gave
// about 2:1, under the 4.5:1 that text is normally held to
TEST(Theme, ValueFillLeavesTheNumberOnItReadable) {
	const Theme& th = Theme::Instance();
	EXPECT_GE(Theme::ContrastRatio(th.text, th.valueFill), 4.5f);
	EXPECT_GE(Theme::ContrastRatio(th.text, th.valueFillActive), 4.5f);
}

// ================================================================
// TOGGLES
// ================================================================

// an engaged toggle is a fill of its own color with its glyph written on it in whichever
// ink reads there, and it has to be plainly not the resting button it was a click ago.
// the floor for the glyph is the 3:1 a control's non-text parts are held to
TEST(Theme, EngagedTogglesKeepTheirGlyphReadableAndStandApart) {
	const Theme& th = Theme::Instance();
	const ImU32 fills[] = {th.accent, th.muteActive, th.soloActive, th.automationActive};
	for (ImU32 fill : fills) {
		const ImU32 ink = Theme::ContrastingText(fill, th.text, th.textOnAccent);
		EXPECT_GE(Theme::ContrastRatio(ink, fill), 3.0f);
		EXPECT_GT(Theme::ContrastRatio(fill, th.button), 1.5f);
	}
}

// ================================================================
// NEUTRAL SURFACES
// ================================================================

// the surfaces and the text on them carry no hue of their own - exact grays - so the
// only color on screen is color that means something: the accent, a track's color, a
// toggle that is on, a meter going red
TEST(Theme, SurfacesAndTextAreNeutralGray) {
	const Theme& th = Theme::Instance();
	const ImU32 neutrals[] = {th.bgDeepest, th.bgChrome, th.inputBg, th.bgWindow, th.bgHeader, th.bgPanel, th.bgPanelAlt,
							  th.bgHover, th.bgActive, th.bgDeviceEffect, th.bgDeviceInstrument, th.bgDeviceRack, th.bgRackInner,
							  th.deviceTitle, th.deviceTitleSelected, th.bgLane, th.bgLaneSelected, th.bgLaneGroup,
							  th.button, th.buttonHover, th.buttonActive, th.border, th.divider,
							  th.text, th.textMuted, th.textDim};
	for (ImU32 color : neutrals) {
		const int r = (int)((color >> IM_COL32_R_SHIFT) & 0xFF);
		const int g = (int)((color >> IM_COL32_G_SHIFT) & 0xFF);
		const int b = (int)((color >> IM_COL32_B_SHIFT) & 0xFF);
		EXPECT_EQ(r, g);
		EXPECT_EQ(g, b);
	}
}
