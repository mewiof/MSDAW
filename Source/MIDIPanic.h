#pragma once
#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>
#include "MIDITypes.h"

// ================================================================
// MIDI PANIC
// ================================================================

// releases everything activeNotes says a plugin is holding: an explicit note-off per held
// note, then all-notes-off (cc 123) and, when allSoundOff is set, all-sound-off (cc 120) -
// the one that also cuts reverb and delay tails, so a loop wrap asks for the soft form and
// a stop or seek for the hard one. activeNotes is left empty
//
// the one place a panic is assembled, shared by VST2 and VST3 hosting. it goes in front of
// whatever the sequencer already put in the block, and that order is the whole point: a
// loop wrap and a seek both land on a position that usually re-triggers a note at sample 0,
// and a release delivered behind that note-on lets go of the note just started - the pass
// sounded once and every one after it fell silent
inline void PrependMIDIPanic(std::vector<MIDIMessage>& mIDIMessages, std::set<int> (&activeNotes)[16], bool allSoundOff) {
	const size_t blockEvents = mIDIMessages.size();

	const auto emit = [&](uint8_t status, uint8_t data1) {
		MIDIMessage msg;
		msg.status = status;
		msg.data1 = data1;
		msg.data2 = 0;
		msg.frameIndex = 0;
		mIDIMessages.push_back(msg);
	};

	for (int ch = 0; ch < 16; ++ch) {
		for (int note : activeNotes[ch])
			emit((uint8_t)(0x80 | ch), (uint8_t)note);
		activeNotes[ch].clear();

		emit((uint8_t)(0xB0 | ch), 123);
		if (allSoundOff)
			emit((uint8_t)(0xB0 | ch), 120);
	}

	// assembled with push_back, then rotated in front of the block's own events, which
	// keep their order behind it
	std::rotate(mIDIMessages.begin(), mIDIMessages.begin() + (std::ptrdiff_t)blockEvents, mIDIMessages.end());
}
