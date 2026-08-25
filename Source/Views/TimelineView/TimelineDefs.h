#pragma once
#include "Clip.h"
#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include "Undo/Actions.h"
#include "imgui.h"
#include <memory>
#include <map>
#include <vector>

// shared payload for drag-and-drop of devices
struct DeviceMovePayload {
	int trackIndex;
	int deviceIndex;
};

struct PendingClipMove {
	std::shared_ptr<Clip> clip;
	int fromTrackIdx;
	int toTrackIdx;
	double newStartBeat;
	bool valid = false;
};

struct PendingClipDelete {
	std::shared_ptr<Clip> clip;
	int trackIdx;
	bool valid = false;
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

	// undo: dragged track's clip state captured at drag start
	std::vector<ClipSnapshotAction::Entry> dragClipsBefore;

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

	// automation selection state (marquee)
	bool autoMarqueeActive = false;
	ImVec2 autoMarqueeStart = {0, 0};
	ImVec2 autoMarqueeCurrent = {0, 0};

	// ruler selection
	double selectionDragStart = 0.0;

	// clipboard
	std::shared_ptr<Clip> clipboard;

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
