#pragma once
#include "EditorContext.h"
#include "Clips/AudioClip.h"
#include "Parameters/KnobParameter.h"
#include "imgui.h"
#include <memory>
#include <string>

// everything an edit to a clip field needs to go through: the lock it has to take, the
// stack its undo entry lands on, the clip being edited and the tempo a warp edit is
// validated against. the clip view refills it once a frame and every field reads it,
// because a Parameter has no way back to any of them on its own
struct ClipEditContext {
	Project* project = nullptr;
	UndoManager* undoManager = nullptr;
	std::shared_ptr<AudioClip> clip;
	double projectBpm = 120.0;
};

// one warp/pitch field of the selected audio clip, drawn either as the value box the rest
// of the app is edited with or as a dial (drag, type a number, double-click to reset). the
// clip owns the value, not this object: it is pulled in fresh every frame - so picking
// another clip, or an undo that lands elsewhere, simply shows up - and written back under
// the project lock as the field is dragged. the finished edit is recorded as an
// AudioClipWarpAction, the same step the buttons beside it push; the usual
// ParameterChangeAction would restore this mirror and leave the clip untouched, on top of
// pointing at whichever clip happened to be selected at the time
class AudioClipParameter : public KnobParameter {
public:
	// step > 0 makes the field publish whole steps only (a semitone, a cent), so what the
	// box prints and what the clip plays cannot disagree
	AudioClipParameter(const ClipEditContext& edit, const std::string& name,
					   float defaultValue, float minValue, float maxValue,
					   double (AudioClip::*read)() const, void (AudioClip::*write)(double),
					   const char* valueFmt, const char* actionName,
					   float step = 0.0f, ImGuiKnobVariant variant = ImGuiKnobVariant_Linear)
		: KnobParameter(name, defaultValue, minValue, maxValue, variant),
		  mEdit(edit), mRead(read), mWrite(write), mValueFmt(valueFmt), mActionName(actionName), mStep(step) {}

	// width <= 0 spends whatever room is left on the line
	bool DrawField(float width) { return DrawTracked(width, 0.0f); }

	// the same field as a dial, for the control an arrangement is actually tuned from;
	// width <= 0 sizes it to its own contents
	bool DrawKnob(float radius, float width = 0.0f) { return DrawTracked(width, radius); }

	bool Draw() override { return DrawField(0.0f); }
protected:
	void CommitEdit(float oldValue, float newValue) override;

	// a stepped field drags on an unsnapped accumulator and snaps only what it publishes:
	// measuring the next delta from the snapped value would swallow every movement smaller
	// than one step, and the drag would stall until a frame happened to cross a whole one
	float NormalizedFromValue() const override;
	void SetValueFromNormalized(float t) override;

	// one unit for the field whichever widget drew it - a dial picks its own formatting
	// from the knob variant, and a clip field has exactly one unit to read in
	void FormatValue(char* buffer, size_t bufferSize, const char* valueFmt) const override;
private:
	// the shared half of both draws: a radius above zero picks the dial, otherwise the box
	bool DrawTracked(float width, float knobRadius);
	void WriteToClip();

	const ClipEditContext& mEdit;
	double (AudioClip::*mRead)() const;
	void (AudioClip::*mWrite)(double);
	const char* mValueFmt;
	const char* mActionName;
	float mStep; // 0 for a continuous field

	// one drag = one undo entry, so the state the gesture started from is kept until it
	// ends. an instant edit (a typed value, a double-click reset) has no gesture and
	// undoes from the state read at the top of the frame it happened on instead
	AudioClipWarpState mGestureBefore;
	bool mGestureActive = false;
	bool mCommitPending = false;

	// unsnapped drag accumulator, only live while a stepped field is being dragged
	float mRawValue = 0.0f;
};

class ClipView {
public:
	ClipView(EditorContext& context);
	void Render(const ImVec2& pos, float width, float height);
private:
	EditorContext& mContext;

	// refilled at the top of every frame that draws an audio clip; the fields below
	// read it through the reference they were built with
	ClipEditContext mEdit;

	AudioClipParameter mSegmentBpm;
	AudioClipParameter mTransientEnvelope;
	AudioClipParameter mGrainSize;
	AudioClipParameter mFluctuation;
	AudioClipParameter mFormants;
	AudioClipParameter mTransposeSemitones;
	AudioClipParameter mTransposeCents;
};
