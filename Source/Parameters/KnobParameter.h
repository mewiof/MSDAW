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

	bool Draw() override;
protected:
	// a Hertz knob travels logarithmically and prints kHz / dB / ms rather than a bare
	// number. both the dial and the compact box go through these, so the same parameter
	// cannot end up behaving differently depending on which widget drew it
	float NormalizedFromValue() const override;
	void SetValueFromNormalized(float t) override;
	void FormatValue(char* buffer, size_t bufferSize, const char* valueFmt) const override;
};
