#pragma once
#include "EditorContext.h"
#include "TimelineDefs.h"
#include "Track.h"
#include <memory>
#include <vector>

class Project;

// ================================================================
// TIMELINE CLIP OPERATIONS
// ================================================================
// everything that acts on the clip selection as a block. the renderer, the track
// view and the arrangement's keyboard shortcuts all reach the same code here, so
// "duplicate" from the context menu and Ctrl+D behave identically on one clip and
// on twenty. each entry point is responsible for its own project lock and for
// pushing one undo step covering every track it touched
namespace TimelineClipOps {

// a selected clip together with the track it currently lives on. the owning track
// is resolved by scanning rather than stored, because a track index shifts under
// reorder/group and the selection outlives those
struct ClipRef {
	std::shared_ptr<Clip> clip;
	std::shared_ptr<Track> track;
	int trackIndex = -1;
};

// ---- queries ----

int FindTrackIndex(Project* project, const std::shared_ptr<Clip>& clip);

// the selection resolved against the project, in track order then start-beat order.
// clips that are no longer on any track are silently skipped
std::vector<ClipRef> ResolveSelection(EditorContext& context);

// drop selection entries whose clip has left the project (an undo, a delete from
// somewhere else). without this a stale clip keeps drawing a selection outline that
// belongs to nothing and the piano roll keeps editing a detached note list
void PruneSelection(EditorContext& context);

// ---- selection gestures ----

// shift-click: everything inside the box spanned by the focused clip and the
// clicked one, the way dragging a marquee over the two would select them
void SelectRangeTo(EditorContext& context, const std::shared_ptr<Clip>& target);

// a marquee edge placed from a raw mouse position: clamped to the start of the
// timeline and snapped to the timeline grid, with shift placing it freely. the box is
// only ever a hit test, but it is still drawn over the arrangement, and an edge that
// slid between the grid lines every other gesture rides would read as a bug
double SnapMarqueeBeat(EditorContext& context, double beat);

// clips a marquee overlaps, in track order then start-beat order. trackFrom/trackTo
// and beatFrom/beatTo may come in either order
std::vector<std::shared_ptr<Clip>> ClipsInBox(Project* project, int trackFrom, int trackTo, double beatFrom, double beatTo);

void SelectAll(EditorContext& context);

// ---- edits ----

// each clip's copy lands one selection-span later, so a block of drums duplicates
// into the bars right after itself instead of every clip piling onto its own tail
void DuplicateSelection(EditorContext& context);

void DeleteSelection(EditorContext& context);

// flips every selected clip to the opposite of what the focused one is, so a mixed
// selection resolves to one state instead of scattering further
void ToggleSelectionEnabled(EditorContext& context);

// splits every selected clip that straddles the beat; both halves stay selected
void SplitSelectionAt(EditorContext& context, double beat);

void CopySelection(EditorContext& context, TimelineInteractionState& interaction);

// paste the clipboard block with its top-left corner at (anchorBeat, anchorTrack).
// entries whose target lane is missing or cannot take clips are dropped
void PasteAt(EditorContext& context, TimelineInteractionState& interaction, double anchorBeat, int anchorTrack);

// ---- drag geometry ----

// where one member of the current drag would land. every entry gets the deltas the
// gesture measured on the clip it started from, clamped per clip (audio length, the
// minimum duration, the left edge of the timeline)
DraggedClipGeometry ComputeDragGeometry(EditorContext& context, const TimelineInteractionState& interaction, const DragClipEntry& entry);

// commit the current drag. cross-track moves are handed to pendingMove because they
// erase from vectors the caller is iterating; same-track moves and resizes are
// applied here and pushed as one undo step
void CommitDrag(EditorContext& context, TimelineInteractionState& interaction, PendingClipMove& pendingMove);

} // namespace TimelineClipOps
