#pragma once
#include "Clip.h"
#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include "Undo/Actions.h"
#include "imgui.h"
#include <memory>
#include <map>
#include <vector>

// a committed cross-track drag. it is deferred out of the clip loop because moving a
// clip erases from one track's vector and pushes onto another's, both of which the
// loop is iterating. a multi-clip drag lands as one batch so the whole selection
// moves (and undoes) together
struct PendingClipMove {
	struct Entry {
		std::shared_ptr<Clip> clip;
		int fromTrackIdx;
		int toTrackIdx;
		double newStartBeat;
	};
	std::vector<Entry> entries;
	bool valid = false;
};

struct PendingClipDelete {
	struct Entry {
		std::shared_ptr<Clip> clip;
		int trackIdx;
	};
	std::vector<Entry> entries;
	bool valid = false;
};

// one clip travelling with the current drag, captured at drag start. the whole
// selection moves/resizes as a rigid body, so every entry gets the same deltas
// measured off the clip the gesture actually started on
struct DragClipEntry {
	std::shared_ptr<Clip> clip;
	int trackIdx;
	double startBeat;
	double duration;
	double offset;
};

// where a dragged clip would land, derived from the gesture's deltas. the ghost
// preview and the commit both read this, so what is drawn is what gets applied
struct DraggedClipGeometry {
	int trackIdx = -1;
	double start = 0.0;
	double duration = 0.0;
	double offset = 0.0;
};

// one clip on the timeline clipboard. positions are stored relative to the copied
// block's top-left corner (its earliest start beat, its topmost track), so a paste
// only has to pick an anchor and the block keeps its internal shape
struct ClipboardClip {
	std::shared_ptr<Clip> clip; // a detached clone, never a clip that is on a track
	double beatOffset = 0.0;
	int trackOffset = 0;
};

enum class DragState {
	None,
	Moving,
	ResizingLeft,
	ResizingRight
};

// consolidated interaction state to pass between view, ruler, and tracks
struct TimelineInteractionState {
	// clip dragging
	DragState dragState = DragState::None;
	double dragOriginalStart = 0.0;
	double dragOriginalDuration = 0.0;
	double dragOriginalOffset = 0.0;

	// live dragging state (preview)
	int dragSourceTrackIdx = -1;
	int dragTargetTrackIdx = -1;

	// dynamic values calculated during drag
	double dragCurrentBeat = 0.0;
	double dragCurrentDuration = 0.0;
	double dragCurrentOffset = 0.0;

	// every clip moving with this drag (the whole clip selection), plus the tracks
	// they started on so the commit can snapshot exactly what it is about to change
	std::vector<DragClipEntry> dragEntries;
	std::vector<int> dragTrackIndices;

	// has this drag actually travelled, or is the mouse merely held down on a clip.
	// the ghosts and the dimmed originals only appear once it has, so a plain click
	// does not flash the whole selection out and straight back in
	bool dragMoved = false;

	// a plain click on a clip that is already part of a multi-selection collapses the
	// selection down to it - but only on release, so click-and-drag still moves the
	// whole block. remembered here between the press and the release
	std::shared_ptr<Clip> dragCollapseCandidate;

	// automation dragging
	int autoDragTrackIndex = -1;
	int autoDragPointIndex = -1;
	bool autoDragIsTension = false;
	float dragStartY = 0.0f;
	float dragStartVal = 0.0f;

	// automation dragging multiple points: maps index -> original state (beat, value)
	std::map<int, std::pair<double, float>> autoDragInitialStates;

	// mouse position (unsnapped beat, unclamped value) when a point drag began. deltas are
	// measured from here rather than from the point itself so grabbing a point off-centre --
	// or adding one on the curve under a cursor that sits a few pixels off it -- doesn't
	// teleport the point to the cursor
	double autoDragAnchorBeat = 0.0;
	float autoDragAnchorVal = 0.0f;

	// undo: automation curve captured at the start of an edit gesture
	std::vector<AutomationPoint> autoEditBefore;

	// point targeted by the automation right-click context menu (for exact value entry)
	// the local closestIdx is recomputed from the mouse each frame, so it can't be trusted
	// while the popup lives across frames
	int autoContextPointIndex = -1;

	// tension handle targeted by the right-click context menu, and the beat the menu was
	// opened at. the live mouse sits over the popup once it is up, so paste needs the
	// remembered beat instead
	int autoContextTensionIndex = -1;
	double autoContextBeat = 0.0;

	// automation selection state (marquee). it is a time range, so it is held as grid-snapped
	// beats rather than pixels -- zooming or scrolling mid-drag can't drift it that way
	bool autoMarqueeActive = false;
	double autoMarqueeStartBeat = 0.0;
	double autoMarqueeEndBeat = 0.0;

	// clip marquee (rubber band) dragged over empty lane space. held as beats and
	// track indices rather than pixels so scrolling or zooming mid-drag cannot drift
	// it, and both edges ride the grid (hold shift to place them freely) - the same
	// rules the automation lane's marquee follows
	bool clipMarqueeActive = false;
	double clipMarqueeStartBeat = 0.0;
	double clipMarqueeEndBeat = 0.0;
	int clipMarqueeStartTrack = -1;
	int clipMarqueeEndTrack = -1;
	bool clipMarqueeMoved = false;
	// selection the marquee started from, so a Ctrl/Shift-drag adds to it instead of
	// replacing it and a shrinking box gives back what it never covered
	std::vector<std::shared_ptr<Clip>> clipMarqueeBase;

	// ruler selection
	double selectionDragStart = 0.0;

	// clipboard
	std::vector<ClipboardClip> clipboard;

	// renaming
	std::shared_ptr<Clip> clipToRename = nullptr;
	bool triggerRenamePopup = false;
	char renameBuffer[256] = "";
};

// helper to clone clips
inline std::shared_ptr<Clip> CloneClip(std::shared_ptr<Clip> source) {
	if (!source)
		return nullptr;
	if (auto ac = std::dynamic_pointer_cast<AudioClip>(source)) {
		return std::make_shared<AudioClip>(*ac);
	}
	if (auto mc = std::dynamic_pointer_cast<MIDIClip>(source)) {
		return std::make_shared<MIDIClip>(*mc);
	}
	return nullptr;
}
