#pragma once

#include "ContinuousParameter.h"

enum ImGuiKnobVariant {
	ImGuiKnobVariant_Linear,		// 0 to 100, -10 to +10, etc
	ImGuiKnobVariant_Percent,		// 0% to 100%
	ImGuiKnobVariant_Hertz,			// logarithmic (20Hz ... 20kHz)
	ImGuiKnobVariant_Decibel,		 // linear, formatted as dB, fills from left
	ImGuiKnobVariant_DecibelBipolar, // linear, formatted as dB, fills from center
	ImGuiKnobVariant_Milliseconds	 // linear, formatted as ms (envelope times)
};

class KnobParameter : public ContinuousParameter {
public:
	ImGuiKnobVariant variant;

	KnobParameter(const std::string& name, float value, float minValue, float maxValue, ImGuiKnobVariant variant = ImGuiKnobVariant_Linear)
		: ContinuousParameter(name, value, minValue, maxValue), variant(variant) {}

	// the dial every knob in the app is drawn at. a device that sizes its own knobs
	// treats this as the ceiling rather than a starting point: a knob that grew to fill
	// its column would be the only oversized control on screen
	static constexpr float kDefaultRadius = 18.0f;

	bool Draw() override;

	// the same dial at an explicit size, for a layout that has to fit a fixed height:
	// the device rack is one short strip, and three stacked knobs at the default radius
	// are taller than all of it. width <= 0 sizes the block to its own contents
	//
	// label overrides what is printed over the dial, for a device whose parameter names
	// carry more than the column has room for (a band's "Freq 1A" reads as "Freq" once
	// the strip below already says which band is selected). the parameter keeps its own
	// name: that is what automation is bound and serialized by
	bool DrawSized(float radius, float width = 0.0f, const char* label = nullptr);

	// how tall DrawSized comes out at that radius - a name over the dial over its value
	static float SizedHeight(float radius);

	// the radius that fits a block of `height`, or 0 when no usable dial fits at all
	static float RadiusForHeight(float height);
protected:
	// a Hertz knob travels logarithmically and prints kHz / dB / ms rather than a bare
	// number. both the dial and the compact box go through these, so the same parameter
	// cannot end up behaving differently depending on which widget drew it
	float NormalizedFromValue() const override;
	void SetValueFromNormalized(float t) override;
	void FormatValue(char* buffer, size_t bufferSize, const char* valueFmt) const override;
};
