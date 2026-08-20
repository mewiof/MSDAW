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
};
