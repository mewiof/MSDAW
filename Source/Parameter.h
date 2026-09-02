#pragma once
#include <string>
#include <functional>

class Parameter {
public:
	std::string name;
	float value;
	float minValue;
	float maxValue;
	float defaultValue;

	Parameter(const std::string& name, float value, float minValue, float maxValue)
		: name(name), value(value), minValue(minValue), maxValue(maxValue), defaultValue(value) {}

	virtual ~Parameter() = default;

	// returns true if value changed
	virtual bool Draw() = 0;

	// compact single-row variant for tight horizontal strips (e.g. the transport bar):
	// a fixed-width framed value box, no label line. base default is a no-op so this can
	// be called through a Parameter* even for subclasses that do not implement it
	// drawFill paints a left-anchored level bar behind the value (used by the track mixer)
	// a null valueFmt means "whatever unit this parameter reads best in" - a knob prints
	// its own kHz / dB / ms formatting, so a caller does not have to know which
	virtual bool DrawCompact(float width, const char* valueFmt, bool drawFill = false) { return false; }

	// e.g., double-click to reset or right-click for the context menu
	bool HandleCommonInteractions();

	void ResetToDefault() { value = defaultValue; }

	static Parameter* GetAndClearAutomationRequestParameter();

	bool IsSelected() const { return sSelectedParameter == this; }
	void Select();

	// the parameter the user last clicked, which is what a rack's map mode maps onto a
	// macro. null once a click has landed anywhere else
	static Parameter* GetSelectedParameter() { return sSelectedParameter; }

	// clears the typed-value selection when a frame's click landed on anything other
	// than a parameter (empty space, another widget), so the digit-entry focus can be
	// dismissed by clicking away. call once per frame after every parameter is drawn
	static void ProcessDeselection();

	// ---- edit tracking (undo + "last turned parameter") ----
	// Draw() implementations call these so that a single user edit becomes one
	// undo entry and updates the "last touched" parameter
	//   - drag widgets: BeginEditGesture() on gesture start, EndEditGesture() on end
	//   - instant edits (toggle / typed / reset): CommitEditImmediate(oldValue)
	void BeginEditGesture();
	void EndEditGesture();
	void CommitEditImmediate(float oldValue);

	// editor installs this to record ParameterChangeActions onto the undo stack
	static std::function<void(Parameter* param, float oldValue, float newValue)> sOnEditCommitted;

	// last parameter the user actually changed (drives the "show automation for
	// last parameter" button)
	static Parameter* GetLastTouchedParameter() { return sLastTouchedParameter; }

	// called when a plugin's OWN editor window reports a parameter change (VST2
	// audioMasterAutomate / VST3 performEdit). Marks it as the last touched
	// parameter so "Show Auto" works for plugin knobs too. Coalesced undo for
	// these is handled via BeginEditGesture/EndEditGesture from the plugin's
	// begin/end-edit callbacks
	static void NotifyExternalEdit(Parameter* param) {
		if (param)
			sLastTouchedParameter = param;
	}

	// request that the editor reveal this parameter's automation lane
	static void RequestAutomation(Parameter* param) { sAutomationRequestParameter = param; }
protected:
	// how a finished edit is recorded, called once per edit by both commit paths. the
	// default hands it to sOnEditCommitted, which the editor turns into a
	// ParameterChangeAction writing straight back into this object - correct for a
	// parameter that IS the value. a parameter that only MIRRORS a value owned
	// elsewhere overrides this: restoring the mirror does not restore what it mirrors,
	// and by the time an undo runs the mirror may be pointing at something else
	virtual void CommitEdit(float oldValue, float newValue);

	// true while this parameter is the one inside a Begin/EndEditGesture pair
	bool IsInEditGesture() const { return sEditingParam == this; }

	static Parameter* sAutomationRequestParameter;
	static Parameter* sSelectedParameter;

	// ImGui frame in which Select() was last called. lets ProcessDeselection tell a
	// click that (re)selected a parameter apart from a click that missed all of them
	static int sSelectFrame;

	// edit-gesture state
	static Parameter* sEditingParam;
	static float sEditOldValue;
	static Parameter* sLastTouchedParameter;
};
