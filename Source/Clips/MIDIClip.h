#pragma once
#include "Clip.h"
#include <cstdint>
#include <vector>
#include <string>
#include <memory>

struct MIDINote {
	int noteNumber;
	int velocity;
	double startBeat;
	double durationBeats;

	// memberwise compare so undo can tell whether a note edit actually changed anything
	bool operator==(const MIDINote&) const = default;
};

// hands out the next never-yet-used sequence id. ids are what a save writes and a
// load groups by, so two of them must never collide inside one session
uint32_t NextMIDISequenceId();

// the notes themselves, held apart from the clip that plays them. duplicating,
// splitting or overlap-trimming a MIDI clip hands the copy the SAME sequence, so the
// two are "linked" (non-unique) and an edit to either lands on both, until MakeUnique
// gives one a sequence of its own. the id travels through the project file so clips
// that shared a sequence still share one after a reload
struct MIDISequence {
	std::vector<MIDINote> notes;
	uint32_t id = NextMIDISequenceId();
};

class MIDIClip : public Clip {
public:
	MIDIClip();
	~MIDIClip() override = default;

	bool LoadFromFile(const std::string& path);

	// notes accessors
	void AddNote(const MIDINote& note) {
		if (mSequence)
			mSequence->notes.push_back(note);
	}

	const std::vector<MIDINote>& GetNotes() const { return mSequence->notes; }
	std::vector<MIDINote>& GetNotesEx() { return mSequence->notes; }

	// check if clips share data
	bool IsLinkedTo(const MIDIClip& other) const {
		return mSequence == other.mSequence;
	}

	// true while at least one other clip plays these same notes. the timeline badges
	// those, because editing one of them silently rewrites the others
	bool IsSequenceShared() const { return mSequence && mSequence.use_count() > 1; }

	uint32_t GetSequenceId() const { return mSequence ? mSequence->id : 0; }

	// break links and create unique notes
	void MakeUnique();

	// ---- link restoration after a load ----
	// every clip parses its own copy of the notes, so the sharing has to be rebuilt
	// afterwards from the ids that were saved - the same two-pass shape PARENT_IDX
	// uses for the track hierarchy. Project::Load drives it once all tracks exist
	uint32_t GetLoadedSequenceId() const { return mLoadedSequenceId; }
	void AdoptSequence(const std::shared_ptr<MIDISequence>& sequence) {
		if (sequence)
			mSequence = sequence;
	}
	const std::shared_ptr<MIDISequence>& GetSequence() const { return mSequence; }

	void Save(std::ostream& out) override;
	void Load(std::istream& in) override;
private:
	// replaced mnotes with a shared pointer
	std::shared_ptr<MIDISequence> mSequence;

	// sequence id read off the project file, resolved against the other clips once
	// the whole project is in memory. 0 for a clip that was never loaded
	uint32_t mLoadedSequenceId = 0;
};
