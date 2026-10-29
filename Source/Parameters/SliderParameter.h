#pragma once

#include "ContinuousParameter.h"

class SliderParameter : public ContinuousParameter {
public:
	// bipolar: the value swings both ways from zero (a pan), so its level fill grows out
	// of the zero point instead of in from the bottom of the range
	SliderParameter(const std::string& name, float value, float minValue, float maxValue, bool bipolar = false)
		: ContinuousParameter(name, value, minValue, maxValue), mBipolar(bipolar) {}

	bool Draw() override;
protected:
	float FillOrigin() const override;
private:
	bool mBipolar = false;
};
