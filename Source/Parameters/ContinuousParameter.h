#pragma once

#include <cstddef>

#include "Parameter.h"

class ContinuousParameter : public Parameter {
public:
	ContinuousParameter(const std::string& name, float value, float minValue, float maxValue)
		: Parameter(name, value, minValue, maxValue) {}

	virtual ~ContinuousParameter() = default;

	// one-row framed value box (drag / type-to-enter / undo / reset), no label line
	bool DrawCompact(float width, const char* valueFmt, bool drawFill = false) override;
protected:
	// the widgets drag in normalized space and let the parameter decide what that maps
	// to, so a compact box and a dial of the same parameter travel identically. linear
	// here; KnobParameter overrides the pair to keep a Hertz control logarithmic
	virtual float NormalizedFromValue() const;
	virtual void SetValueFromNormalized(float t);

	// how the value reads inside the box. valueFmt is a printf format taking one float,
	// or null to let the parameter pick its own units
	virtual void FormatValue(char* buffer, size_t bufferSize, const char* valueFmt) const;

	// typing interception
	void CheckTypingStart(ImGuiID currentID);
	bool IsTyping(ImGuiID currentID) const;
	bool DrawTypingInput(ImGuiID currentID, float width, float yOffset = 0.0f);

	// dragging and screen-wrap
	float GetSafeMouseDeltaY() const;
	void HandleInfiniteDrag();
	void RestoreMousePosition();
private:
	static ImGuiID s_TypingID;
	static char s_TextBuffer[64];
	static bool s_FocusNextFrame;
	static bool s_MoveCursorToEnd;
};
