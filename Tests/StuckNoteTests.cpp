#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "Clips/MIDIClip.h"
#include "Track.h"

// ================================================================
// STUCK NOTES
// ================================================================

// the sequencer is stateless: every block it recomputes each note's on and off sample
// from the clip data and fires whatever lands in the window. so every release is
// derived from the data rather than from what was actually played, and any change to
// that data in between orphaned the note-on - the instrument held the key forever.
// Track keeps a ledger of what it has sounded and reconciles it against the clip data
// every block; these pin down each way the data can move out from under a live note.
// what reaches the instrument is what matters, so the assertions are on the emitted
// MIDI rather than on the ledger itself

namespace {

	constexpr int kNumFrames = 128;
	constexpr int kNumChannels = 2;
	constexpr double kSampleRate = 48000.0;
	constexpr double kBpm = 120.0;
	constexpr double kSamplesPerBeat = kSampleRate * 60.0 / kBpm;

	constexpr uint8_t kNoteOn = 0x90;
	constexpr uint8_t kNoteOff = 0x80;

	// a four-beat clip at bar 1 holding one note that runs the whole length of it.
	// built in place rather than returned from a helper: Track carries atomics for its
	// peak meters, so it is neither copyable nor movable
	struct Fixture {
		Track track;
		std::shared_ptr<MIDIClip> clip;
		int64_t playhead = 0;

		explicit Fixture(int noteNumber) {
			clip = std::make_shared<MIDIClip>();
			clip->SetStartBeat(0.0);
			clip->SetDuration(4.0);
			clip->AddNote({noteNumber, 100, 0.0, 4.0});
			track.AddClip(clip);
		}
	};

	// render one block and hand back the MIDI the sequencer produced for it
	std::vector<MIDIMessage> RenderBlock(Fixture& f, bool playheadJumped = false, double bpm = kBpm) {
		std::vector<MIDIMessage> messages;
		std::vector<float> buffer(kNumFrames * kNumChannels, 0.0f);

		ProcessContext context;
		context.sampleRate = kSampleRate;
		context.bpm = bpm;
		context.currentSample = f.playhead;
		context.isPlaying = true;
		context.playheadJumped = playheadJumped;

		f.track.Process(buffer.data(), kNumFrames, kNumChannels, messages, context);
		f.playhead += kNumFrames;
		return messages;
	}

	void RenderUntil(Fixture& f, int64_t sample) {
		while (f.playhead < sample)
			RenderBlock(f);
	}

	int CountOf(const std::vector<MIDIMessage>& messages, uint8_t status, int noteNumber) {
		int count = 0;
		for (const auto& m : messages) {
			if (m.status == status && m.data1 == (uint8_t)noteNumber)
				++count;
		}
		return count;
	}

	bool Contains(const std::vector<MIDIMessage>& messages, uint8_t status, int noteNumber) {
		return CountOf(messages, status, noteNumber) > 0;
	}

	// get the note sounding, and prove it actually started before anything else is asserted
	void StartTheNote(Fixture& f, int noteNumber) {
		ASSERT_TRUE(Contains(RenderBlock(f), kNoteOn, noteNumber)) << "the fixture never sounded the note";
	}

} // namespace

TEST(StuckNotes, DraggingASoundingNoteToAnotherPitchReleasesThePitchItStartedOn) {
	Fixture f(60);
	StartTheNote(f, 60);

	// the piano roll moving the note up an octave mid-playback. the release the
	// sequencer goes on to compute is for 72 - nothing would ever have let go of 60
	f.clip->GetNotesEx()[0].noteNumber = 72;

	EXPECT_TRUE(Contains(RenderBlock(f), kNoteOff, 60));
}

TEST(StuckNotes, DeletingASoundingNoteReleasesIt) {
	Fixture f(60);
	StartTheNote(f, 60);

	// with the note gone there is no longer any data from which to compute its release
	f.clip->GetNotesEx().clear();

	EXPECT_TRUE(Contains(RenderBlock(f), kNoteOff, 60));
}

TEST(StuckNotes, DeactivatingAClipReleasesTheNotesItWasSounding) {
	Fixture f(60);
	StartTheNote(f, 60);

	// pressing 0 on a clip that is mid-note: the sequencer skips a deactivated clip
	// wholesale, so it never reaches the note-off inside it
	f.clip->SetEnabled(false);

	EXPECT_TRUE(Contains(RenderBlock(f), kNoteOff, 60));
}

TEST(StuckNotes, MovingAClipOutFromUnderThePlayheadReleasesItsNotes) {
	Fixture f(60);
	StartTheNote(f, 60);

	// dragging the clip somewhere else in the arrangement while it is playing
	f.clip->SetStartBeat(64.0);

	EXPECT_TRUE(Contains(RenderBlock(f), kNoteOff, 60));
}

TEST(StuckNotes, DraggingAClipsEndBehindThePlayheadReleasesItsNotes) {
	Fixture f(60);
	StartTheNote(f, 60);
	RenderUntil(f, 20000);

	// the right edge dragged in to a sixteenth, which now ends long before the playhead
	f.clip->SetDuration(0.25);

	EXPECT_TRUE(Contains(RenderBlock(f), kNoteOff, 60));
}

TEST(StuckNotes, ShorteningAClipReleasesItsNoteAtTheNewEndAndOnlyThere) {
	Fixture f(60);
	StartTheNote(f, 60);

	// the right edge dragged in to half a beat, still ahead of the playhead. a note is
	// gated at its clip's boundary, so this release is the sequencer's own scheduled one
	// and the ledger must not add a second at the head of the following block
	f.clip->SetDuration(0.5);
	int64_t clipEnd = (int64_t)(0.5 * kSamplesPerBeat);

	int releases = 0;
	while (f.playhead < clipEnd + kNumFrames * 2)
		releases += CountOf(RenderBlock(f), kNoteOff, 60);

	EXPECT_EQ(releases, 1);
}

TEST(StuckNotes, JumpingThePlayheadAwayFromASoundingNoteReleasesIt) {
	Fixture f(60);
	StartTheNote(f, 60);

	// Project::ProcessBlock resets a track on a seek it detects, but the UI writes the
	// transport position without the project lock, so a seek can land inside a block and
	// go unnoticed. the sequencer has to be able to recover on its own
	f.playhead = (int64_t)(64.0 * kSamplesPerBeat);

	EXPECT_TRUE(Contains(RenderBlock(f, /*playheadJumped*/ true), kNoteOff, 60));
}

TEST(StuckNotes, ATempoChangeThatMovesAClipOffThePlayheadReleasesItsNotes) {
	Fixture f(60);
	StartTheNote(f, 60);
	RenderUntil(f, 20000); // well inside the note at the fixture tempo

	// beats convert to samples through the tempo, so raising it drags the whole clip -
	// and the release sample the sequencer would compute - back behind the playhead
	EXPECT_TRUE(Contains(RenderBlock(f, false, kBpm * 8.0), kNoteOff, 60));
}

TEST(StuckNotes, AnUndoThatReplacesTheSequenceReleasesWhatItWasSounding) {
	Fixture f(60);
	StartTheNote(f, 60);

	// NoteEditAction restores a snapshot over the live vector; the note that was
	// sounding need not survive that
	f.clip->GetNotesEx() = std::vector<MIDINote>{{67, 100, 8.0, 1.0}};

	EXPECT_TRUE(Contains(RenderBlock(f), kNoteOff, 60));
}

TEST(StuckNotes, AnUntouchedNoteIsNotReleasedWhileItIsStillSounding) {
	Fixture f(60);
	StartTheNote(f, 60);

	// the reconcile must not fire on a note that is simply still held: four beats is
	// far longer than the twenty blocks played here
	for (int block = 0; block < 20; ++block) {
		ASSERT_FALSE(Contains(RenderBlock(f), kNoteOff, 60)) << "released early on block " << block;
	}
}

TEST(StuckNotes, TheScheduledReleaseIsNotDoubledByTheLedger) {
	Fixture f(60);
	StartTheNote(f, 60);

	// play a beat past the end of the note and count every release it produced. the
	// ledger must recognise the note-off the sequencer already emitted rather than
	// adding a second one at the head of the next block
	int releases = 0;
	while (f.playhead < (int64_t)(5.0 * kSamplesPerBeat))
		releases += CountOf(RenderBlock(f), kNoteOff, 60);

	EXPECT_EQ(releases, 1);
}

TEST(StuckNotes, AReleasedNoteIsNotReleasedAgainOnEveryLaterBlock) {
	Fixture f(60);
	StartTheNote(f, 60);
	f.clip->GetNotesEx().clear();

	EXPECT_TRUE(Contains(RenderBlock(f), kNoteOff, 60));
	// the ledger is cleared by the release, so the blocks after it stay silent
	for (int block = 0; block < 5; ++block) {
		ASSERT_FALSE(Contains(RenderBlock(f), kNoteOff, 60)) << "re-released on block " << block;
	}
}

TEST(StuckNotes, APanicClearsWhatTheSequencerBelievesIsSounding) {
	Fixture f(60);
	StartTheNote(f, 60);

	// Reset is the hard panic Project::ProcessBlock runs on a stop or a detected seek:
	// the instruments have already let go, so the sequencer must not chase them with a
	// release of its own on the next block
	f.track.Reset();
	f.clip->GetNotesEx().clear();

	EXPECT_FALSE(Contains(RenderBlock(f), kNoteOff, 60));
}
