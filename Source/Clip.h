#pragma once
#include <string>
#include <iostream>

// the shortest clip the timeline can still draw and grab. an audio clip's length is
// derived from the file, the tempo and the transpose, so it can be squeezed towards zero,
// and a clip of no length at all is one the user can neither see nor select to delete
inline constexpr double kMinClipDurationBeats = 1.0 / 64.0;

class Clip {
public:
	virtual ~Clip() = default;

	void SetStartBeat(double beat) { mStartBeat = beat; }
	double GetStartBeat() const { return mStartBeat; }

	void SetDuration(double beats) { mDuration = beats; }
	double GetDuration() const { return mDuration; }

	// clip source offset in beats
	void SetOffset(double offset) { mOffset = offset; }
	double GetOffset() const { return mOffset; }

	double GetEndBeat() const { return mStartBeat + mDuration; }

	void SetName(const std::string& name) { mName = name; }
	const std::string& GetName() const { return mName; }

	// a deactivated clip keeps its place on the timeline but is skipped by the
	// sequencer, so muting one part of an arrangement never means moving clips out
	// of the way and back
	void SetEnabled(bool enabled) { mEnabled = enabled; }
	bool IsEnabled() const { return mEnabled; }

	// per-clip grid snapping settings
	void SetGrid(int num, int den) {
		mGridNumerator = num;
		mGridDenominator = den;
	}
	int GetGridNumerator() const { return mGridNumerator; }
	int GetGridDenominator() const { return mGridDenominator; }

	virtual void Save(std::ostream& out) {
		out << "CLIP_NAME \"" << mName << "\"\n";
		out << "START " << mStartBeat << "\n";
		out << "DUR " << mDuration << "\n";
		out << "OFFSET " << mOffset << "\n";
		out << "ENABLED " << (mEnabled ? 1 : 0) << "\n";
	}

	virtual void Load(std::istream& in) {
		// parsing handled in subclasses
	}
protected:
	double mStartBeat = 0.0;
	double mDuration = 4.0; // 1 bar
	double mOffset = 0.0;	// content offset
	std::string mName = "Clip";
	bool mEnabled = true;

	int mGridNumerator = 1;
	int mGridDenominator = 4;
};
