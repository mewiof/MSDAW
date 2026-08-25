#pragma once
#include "Track.h"
#include <vector>

// ================================================================
// AUTOMATION EDITS
// ================================================================
// pure edits over a curve's point list, kept out of the timeline renderer so the
// clipboard rules can be driven headlessly. Track::AddAutomationPoint is deliberately
// not used by any of these: it constructs {beat, value, 0.0f} and so drops the tension
// every one of these has to carry
namespace AutomationEdits {

// beats closer together than this are the same position as far as editing goes
inline constexpr double kBeatEpsilon = 0.0001;

void SortByBeat(std::vector<AutomationPoint>& points);

// index of the point sitting on beat, or -1
int IndexAtBeat(const std::vector<AutomationPoint>& points, double beat);

// drop every point in [fromBeat, toBeat]
void EraseRange(std::vector<AutomationPoint>& points, double fromBeat, double toBeat);

// the selected points, rebased so the earliest sits at beat 0
std::vector<AutomationPoint> CopySelection(const std::vector<AutomationPoint>& points);

// splice the clipboard in at anchorBeat, replacing whatever occupied that span. the
// pasted points come out as the new selection
void PasteAt(std::vector<AutomationPoint>& points, const std::vector<AutomationPoint>& clipboard,
			 double anchorBeat, float minValue, float maxValue);

// repeat the selection immediately after itself: an exact replica of the selected block,
// every point and every tension, offset by the block's own length. the copies come out as
// the new selection so Ctrl+D chains. gridBeats only covers the degenerate case of a single
// selected point, which spans no length of its own
void DuplicateSelection(std::vector<AutomationPoint>& points, double gridBeats);

void DeleteSelected(std::vector<AutomationPoint>& points);

} // namespace AutomationEdits
