#pragma once
#include "EditorContext.h"
#include "Clips/AudioClip.h"
#include "Parameters/ContinuousParameter.h"
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

// one warp/pitch field of the selected audio clip, behind the same value box the rest of
// the app is edited with (drag, type a number, double-click to reset). the clip owns the
// value, not this object: it is pulled in fresh every frame - so picking another clip, or
// an undo that lands elsewhere, simply shows up - and written back under the project lock
// as the box is dragged. the finished edit is recorded as an AudioClipWarpAction, the same
// step the buttons beside it push; the usual ParameterChangeAction would restore this
// mirror and leave the clip untouched, on top of pointing at whichever clip happened to be
// selected at the time
class AudioClipParameter : public ContinuousParameter {
public:
	AudioClipParameter(const ClipEditContext& edit, const std::string& name,
					   float defaultValue, float minValue, float maxValue,
					   double (AudioClip::*read)() const, void (AudioClip::*write)(double),
					   const char* valueFmt, const char* actionName)
		: ContinuousParameter(name, defaultValue, minValue, maxValue),
		  mEdit(edit), mRead(read), mWrite(write), mValueFmt(valueFmt), mActionName(actionName) {}

	// width <= 0 spends whatever room is left on the line
	bool DrawField(float width);
	bool Draw() override { return DrawField(0.0f); }
protected:
	void CommitEdit(float oldValue, float newValue) override;
private:
	void WriteToClip();

	const ClipEditContext& mEdit;
	double (AudioClip::*mRead)() const;
	void (AudioClip::*mWrite)(double);
	const char* mValueFmt;
	const char* mActionName;

	// one drag = one undo entry, so the state the gesture started from is kept until it
	// ends. an instant edit (a typed value, a double-click reset) has no gesture and
	// undoes from the state read at the top of the frame it happened on instead
	AudioClipWarpState mGestureBefore;
	bool mGestureActive = false;
	bool mCommitPending = false;
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
