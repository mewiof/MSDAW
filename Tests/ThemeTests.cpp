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
