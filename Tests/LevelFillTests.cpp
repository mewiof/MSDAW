#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include "Parameters/KnobParameter.h"
#include "Parameters/SliderParameter.h"

// ================================================================
// LEVEL FILL ORIGIN
// ================================================================

// a value box and a dial fill from where the parameter sits at rest: the bottom of the
// range for a level, the zero point for a control that swings both ways. a centered pan
// used to draw half a bar - reading as "half" of something - with its edge cutting the
// number in two. the widgets ask FillOrigin, so these ask it the same way

namespace {

	// FillOrigin is how the widgets ask, and it is theirs alone; these open it up to the
	// test without opening it to anything else
	struct SliderProbe : SliderParameter {
		using SliderParameter::SliderParameter;
		using SliderParameter::FillOrigin;
	};

	struct KnobProbe : KnobParameter {
		using KnobParameter::KnobParameter;
		using KnobParameter::FillOrigin;
	};

} // namespace

TEST(LevelFill, ALevelFillsFromTheBottomOfItsRange) {
	// a track's volume runs -60..+6 dB around a 0 dB default. it is a level all the same,
	// so it fills from the bottom rather than out of the zero inside its range
	SliderProbe volume("Volume", 0.0f, -60.0f, 6.0f);
	EXPECT_FLOAT_EQ(volume.FillOrigin(), 0.0f);
}

TEST(LevelFill, ABipolarSliderFillsFromItsZeroPoint) {
	SliderProbe pan("Pan", 0.0f, -1.0f, 1.0f, true);
	EXPECT_FLOAT_EQ(pan.FillOrigin(), 0.5f);

	SliderProbe trim("Trim", 0.0f, -12.0f, 6.0f, true);
	EXPECT_NEAR(trim.FillOrigin(), 12.0f / 18.0f, 1e-6f);
}

TEST(LevelFill, AZeroOutsideTheRangeClampsToTheNearestEnd) {
	SliderProbe aboveZero("Above", 5.0f, 1.0f, 10.0f, true);
	EXPECT_FLOAT_EQ(aboveZero.FillOrigin(), 0.0f);

	SliderProbe belowZero("Below", -5.0f, -10.0f, -1.0f, true);
	EXPECT_FLOAT_EQ(belowZero.FillOrigin(), 1.0f);

	// a range of no width has no inside to fill, and must not divide by it
	SliderProbe empty("Empty", 0.0f, 0.0f, 0.0f, true);
	EXPECT_FLOAT_EQ(empty.FillOrigin(), 0.0f);
}

TEST(LevelFill, OnlyTheBipolarKnobVariantsFillFromZero) {
	KnobProbe linearBipolar("Width", 0.0f, -1.0f, 1.0f, ImGuiKnobVariant_LinearBipolar);
	KnobProbe decibelBipolar("Gain", 0.0f, -12.0f, 12.0f, ImGuiKnobVariant_DecibelBipolar);
	KnobProbe percentBipolar("Amount", 0.0f, -100.0f, 100.0f, ImGuiKnobVariant_PercentBipolar);
	EXPECT_FLOAT_EQ(linearBipolar.FillOrigin(), 0.5f);
	EXPECT_FLOAT_EQ(decibelBipolar.FillOrigin(), 0.5f);
	EXPECT_FLOAT_EQ(percentBipolar.FillOrigin(), 0.5f);

	// the same ranges on a one-way variant fill from the bottom, zero inside them or not
	KnobProbe decibel("Thresh", -24.0f, -60.0f, 0.0f, ImGuiKnobVariant_Decibel);
	KnobProbe linear("Curve", 0.4f, -1.0f, 1.0f, ImGuiKnobVariant_Linear);
	KnobProbe hertz("Freq", 1000.0f, 20.0f, 20000.0f, ImGuiKnobVariant_Hertz);
	EXPECT_FLOAT_EQ(decibel.FillOrigin(), 0.0f);
	EXPECT_FLOAT_EQ(linear.FillOrigin(), 0.0f);
	EXPECT_FLOAT_EQ(hertz.FillOrigin(), 0.0f);
}
