#pragma once
#include "Clip.h"

// a clip is a window onto its source material, and the two have different clocks.
// a MIDINote's startBeat is a CONTENT beat - measured from the material's own zero -
// while the timeline, the piano roll grid and the transport all speak in beats of the
// arrangement. cropping the head of a clip (a split, a left-edge resize, an overlap
// trim) moves the window without moving the material, so the two clocks part company
// by the clip's offset and every view that draws note data has to convert between
// them. Track::Process plays a note at `clipStart + startBeat - offset`, which is
// exactly what ToOuterBeat computes with an origin of 0
//
// "outer" is whatever beat space the origin was given in: arrangement beats by
// default, or the piano roll's view beats when the roll hands over its own origin
struct ClipTimeMapping {
	double contentOrigin = 0.0; // where the material's beat 0 falls
	double windowStart = 0.0;	// where the clip itself starts, i.e. what it plays first
	double windowEnd = 0.0;

	static ClipTimeMapping For(const Clip& clip, double originBeat = 0.0) {
		ClipTimeMapping mapping;
		mapping.windowStart = clip.GetStartBeat() - originBeat;
		mapping.windowEnd = mapping.windowStart + clip.GetDuration();
		mapping.contentOrigin = mapping.windowStart - clip.GetOffset();
		return mapping;
	}

	double ToOuterBeat(double contentBeat) const { return contentOrigin + contentBeat; }
	double ToContentBeat(double outerBeat) const { return outerBeat - contentOrigin; }

	// whether the clip's window actually reaches this content beat. a cropped clip
	// still carries the material on either side of its window - the sequencer skips
	// it, and a view draws it outside the clip's band
	bool PlaysContentBeat(double contentBeat) const {
		double outer = ToOuterBeat(contentBeat);
		return outer >= windowStart && outer < windowEnd;
	}
};
