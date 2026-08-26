#pragma once
#include "EditorContext.h"
#include "Clips/MIDIClip.h"
#include "Parameters/SliderParameter.h"
#include "imgui.h"
#include <vector>
#include <map>
#include <memory>

// struct to store initial state of a note before a drag operation begins
struct NoteDragState {
	double originalStart;
	double originalDuration;
	int originalNoteNum;
};

class PianoRollView {
public:
	// one clip on show in the roll. multi-clip editing lays every MIDI clip of the
	// arrangement selection onto a single shared beat grid, each at the position it
	// holds in the arrangement; only the focused one takes edits, the rest are drawn
	// in their track color as context and can be clicked to take the focus
	struct RollClip {
		std::shared_ptr<MIDIClip> clip;
		ImU32 color = 0;		 // owning track's color
		double viewOffset = 0.0; // clip start measured from the view's beat 0
		bool focused = false;
	};

	PianoRollView(EditorContext& context)
		: mContext(context) {
		// per-clip snap grid edited through the same custom parameter widget as the
		// timeline grid; re-seeded from the edited clip each time it changes
		mGridNumParam = std::make_unique<SliderParameter>("Grid Num", 1.0f, 1.0f, 64.0f);
		mGridDenParam = std::make_unique<SliderParameter>("Grid Den", 4.0f, 1.0f, 128.0f);
	}
	void Render();
private:
	EditorContext& mContext;

	// numerator/denominator widgets for the piano-roll snap grid. the edited clip owns
	// the value, so these are re-seeded whenever mLastGridClip stops matching it
	std::unique_ptr<SliderParameter> mGridNumParam;
	std::unique_ptr<SliderParameter> mGridDenParam;
	std::weak_ptr<Clip> mLastGridClip;

	// selection state
	std::vector<int> mSelectedIndices;

	// interaction state
	enum class InteractionMode {
		None,
		Selecting,		// marquee box
		MovingNotes,	// dragging notes
		ResizingNotes,	// resizing notes
		PianoKeyInput,	// clicking the left piano keys
		EditingVelocity // dragging bars in the velocity lane
	};

	InteractionMode mInteraction = InteractionMode::None;

	// for dragging notes (move/resize)
	// maps note index -> initial state
	std::map<int, NoteDragState> mDragInitialStates;

	// marquee selection, stored in canvas-local coords so it survives scrolling
	// mid-drag (screen coords would drift if the view auto-scrolls)
	ImVec2 mMarqueeStart = {0, 0};
	ImVec2 mMarqueeEnd = {0, 0};

	// audio preview state
	int mLastPreviewNote = -1; // -1 means no note playing

	// scroll of the master grid child, cached each frame so the frozen ruler /
	// keys / velocity siblings can draw at a matching pixel offset
	float mScrollX = 0.0f;
	float mScrollY = 0.0f;

	// was the grid hovered last frame. the Ctrl+Wheel zoom is resolved before the
	// grid child begins (so its content size can be declared up front), which is too
	// early to know this frame's hover - so we reuse last frame's result
	bool mGridHoveredLast = false;

	// auto-center: when the focused clip changes we recenter the view on its
	// notes. compared by weak_ptr identity so we never deref a stale clip. the origin
	// is tracked alongside it because adding a clip to the selection can move the
	// view's beat 0 without the focus changing at all
	std::weak_ptr<Clip> mLastCenteredClip;
	double mLastCenterOrigin = 0.0;
	bool mPendingCenter = false;
	float mCenterTargetX = 0.0f;
	float mCenterTargetY = 0.0f;

	// collapsible panels
	bool mVelocityLaneOpen = true;
	bool mMinimapEnabled = true;

	// zoom (unscaled base values; multiplied by mainScale when used). adjusted
	// with Ctrl+Wheel (horizontal) and Ctrl+Shift+Wheel (vertical)
	float mPixelsPerBeat = 100.0f;
	float mNoteHeight = 16.0f;

	// undo gesture: one drag (move/resize/velocity) = one history entry. the
	// note list is snapshotted at gesture start and pushed once on release
	bool mGestureActive = false;
	std::vector<MIDINote> mGestureBefore;
	const char* mGestureName = "Edit notes";

	// helpers
	void ClearSelection() { mSelectedIndices.clear(); }
	bool IsNoteSelected(int index);
	void SelectNote(int index, bool addToSelection);
	void StopPreview();
	// the arrangement selection resolved into drawable clips, sorted by start beat.
	// origin comes back as the arrangement beat the view's x=0 corresponds to
	std::vector<RollClip> CollectClips(double& origin);
	void CenterOnClip(MIDIClip* clip, double viewOffset, float gridW, float gridH);
	void BeginGesture(const std::shared_ptr<MIDIClip>& clip, const char* name);
	void EndGesture(const std::shared_ptr<MIDIClip>& clip);
};
