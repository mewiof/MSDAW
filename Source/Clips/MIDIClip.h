#pragma once
#include "Clip.h"
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

// sequence container
struct MIDISequence {
	std::vector<MIDINote> notes;
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

	// break links and create unique notes
	void MakeUnique();

	void Save(std::ostream& out) override;
	void Load(std::istream& in) override;
private:
	// replaced mnotes with a shared pointer
	std::shared_ptr<MIDISequence> mSequence;
};
