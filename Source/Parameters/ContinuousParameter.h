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

	// a drag carries its own normalized position for the length of the gesture instead of
	// reading it back off the value every frame. a parameter that quantizes (a stage
	// count) would otherwise round each pixel of travel straight back to where it started,
	// and only a flick large enough to clear half a step in one frame would move it at all
	void BeginDragPosition();
	void ApplyDragDelta(float deltaNormalized);

	// typing interception
	void CheckTypingStart(ImGuiID currentID);
	bool IsTyping(ImGuiID currentID) const;
	bool DrawTypingInput(ImGuiID currentID, float width, float yOffset = 0.0f);

	// dragging and screen-wrap
	float GetSafeMouseDeltaY() const;
	void HandleInfiniteDrag();
	void RestoreMousePosition();
private:
	static Parameter* s_DragParam;
	static float s_DragNormalized;

	static ImGuiID s_TypingID;
	static char s_TextBuffer[64];
	static bool s_FocusNextFrame;
	static bool s_MoveCursorToEnd;
};
