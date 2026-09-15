#pragma once
#include <algorithm>

// ================================================================
// ADAPTIVE GRID
// ================================================================

// the musical division the grid falls on when it is left to follow the zoom, rather
// than being pinned to one the user picked. a fixed division is the wrong unit at both
// ends of a zoom range: 1/16 is a smear of lines nobody can aim at when a bar is forty
// pixels wide, and a whole bar is uselessly coarse once one fills the screen
//
// the ladder is the divisions that mean something musically - powers of two either side
// of the beat - and the choice is the finest one still wide enough to see and hit. the
// same value feeds the lines that get drawn and the snapping, so what the grid looks
// like is always what it does
namespace GridScale {

	// a cell narrower than this is not a target the mouse can pick out, and the lines
	// crowd into a texture rather than reading as a grid
	constexpr float kMinCellPixels = 18.0f;

	inline double Adaptive(float pixelsPerBeat, float scale = 1.0f) {
		// in beats, coarsest first: four bars down to a 64th note
		static const double kDivisions[] = {16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625};
		const float minCell = kMinCellPixels * std::max(scale, 0.1f);

		double chosen = kDivisions[0];
		for (double division : kDivisions) {
			if (division * pixelsPerBeat < minCell)
				break;
			chosen = division; // still wide enough: keep going finer
		}
		return chosen;
	}

} // namespace GridScale
