#include "PrecompHeader.h"
#include "AutomationEdits.h"
#include <algorithm>
#include <cmath>

namespace AutomationEdits {

void SortByBeat(std::vector<AutomationPoint>& points) {
	std::sort(points.begin(), points.end(),
			  [](const AutomationPoint& a, const AutomationPoint& b) { return a.beat < b.beat; });
}

int IndexAtBeat(const std::vector<AutomationPoint>& points, double beat) {
	for (int i = 0; i < (int)points.size(); ++i) {
		if (std::abs(points[i].beat - beat) <= kBeatEpsilon)
			return i;
	}
	return -1;
}

void EraseRange(std::vector<AutomationPoint>& points, double fromBeat, double toBeat) {
	points.erase(std::remove_if(points.begin(), points.end(),
								[&](const AutomationPoint& p) { return p.beat >= fromBeat && p.beat <= toBeat; }),
				 points.end());
}

std::vector<AutomationPoint> CopySelection(const std::vector<AutomationPoint>& points) {
	std::vector<AutomationPoint> out;
	double baseBeat = 0.0;
	for (const auto& p : points) {
		if (!p.selected)
			continue;
		// points are kept sorted, so the first one caught is also the earliest
		if (out.empty())
			baseBeat = p.beat;
		AutomationPoint cp = p;
		cp.beat -= baseBeat;
		cp.selected = false;
		out.push_back(cp);
	}
	return out;
}

void PasteAt(std::vector<AutomationPoint>& points, const std::vector<AutomationPoint>& clipboard,
			 double anchorBeat, float minValue, float maxValue) {
	if (clipboard.empty())
		return;

	double span = clipboard.back().beat;
	EraseRange(points, anchorBeat - kBeatEpsilon, anchorBeat + span + kBeatEpsilon);

	for (auto& p : points)
		p.selected = false;

	for (const auto& src : clipboard) {
		AutomationPoint np = src;
		np.beat = anchorBeat + src.beat;
		np.value = std::clamp(np.value, minValue, maxValue);
		np.selected = true;
		points.push_back(np);
	}
	SortByBeat(points);
}

void DuplicateSelection(std::vector<AutomationPoint>& points, double gridBeats) {
	std::vector<AutomationPoint> copies;
	double minBeat = 0.0;
	double maxBeat = 0.0;
	for (const auto& p : points) {
		if (!p.selected)
			continue;
		if (copies.empty()) {
			minBeat = p.beat;
			maxBeat = p.beat;
		}
		minBeat = std::min(minBeat, p.beat);
		maxBeat = std::max(maxBeat, p.beat);
		copies.push_back(p);
	}
	if (copies.empty())
		return;

	// the block's own length is the step. a single point spans nothing, so it falls back to
	// one grid division
	double offset = maxBeat - minBeat;
	if (offset < kBeatEpsilon)
		offset = gridBeats > 0.0 ? gridBeats : 1.0;

	// clear the whole destination, the joint included. the joint is where the block's last
	// point and the copy's first point both want to sit, and one beat holds one value -- the
	// copy has to win it, or the replica would be missing its leading point (and the tension
	// that shapes its first segment). a block that starts and ends on the same value, which
	// is what a dip or a swell is, loses nothing to that
	EraseRange(points, minBeat + offset - kBeatEpsilon, maxBeat + offset + kBeatEpsilon);

	for (auto& p : points)
		p.selected = false;

	for (auto& np : copies) {
		np.beat += offset;
		np.selected = true;
		points.push_back(np);
	}
	SortByBeat(points);
}

void DeleteSelected(std::vector<AutomationPoint>& points) {
	points.erase(std::remove_if(points.begin(), points.end(),
								[](const AutomationPoint& p) { return p.selected; }),
				 points.end());
}

} // namespace AutomationEdits
