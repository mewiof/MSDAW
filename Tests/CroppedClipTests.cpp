#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "ClipTime.h"
#include "Clips/MIDIClip.h"
#include "Track.h"

// ================================================================
// CROPPED CLIPS
// ================================================================

// a clip is a window onto its material, and cropping its head (a split, a left-edge
// resize, an overlap trim) moves the window without moving the material. from then on
// a note's stored beat and the beat it is heard at differ by the clip's offset, and
// every part of the app that puts note data on a beat grid has to make the same
// conversion the sequencer does. ClipTimeMapping is that conversion; these pin it to
// what Track::Process actually emits, so a view drawn through it cannot drift away
// from what is played. what the piano roll then does with the pixels is verified in
// the running app - this is the arithmetic underneath it

namespace {

	constexpr int kNumFrames = 128;
	constexpr int kNumChannels = 2;
	constexpr double kSampleRate = 48000.0;
	constexpr double kBpm = 120.0;
	constexpr double kSamplesPerBeat = kSampleRate * 60.0 / kBpm;

	constexpr uint8_t kNoteOn = 0x90;
	constexpr uint8_t kNoteOff = 0x80;

	// one emitted MIDI event, placed on the arrangement beat it was heard at rather
	// than on a block-relative frame - that is the number a view has to agree with
	struct Event {
		uint8_t status;
		int noteNumber;
		double beat;
	};

	std::vector<Event> Render(Track& track, double fromBeat, double toBeat) {
		std::vector<Event> out;
		std::vector<float> buffer(kNumFrames * kNumChannels, 0.0f);
		int64_t from = (int64_t)(fromBeat * kSamplesPerBeat);
		int64_t to = (int64_t)(toBeat * kSamplesPerBeat);

		for (int64_t playhead = from; playhead < to; playhead += kNumFrames) {
			std::vector<MIDIMessage> messages;
			std::fill(buffer.begin(), buffer.end(), 0.0f);

			ProcessContext context;
			context.sampleRate = kSampleRate;
			context.bpm = kBpm;
			context.currentSample = playhead;
			context.isPlaying = true;
			context.playheadJumped = (playhead == from);

			track.Process(buffer.data(), kNumFrames, kNumChannels, messages, context);
			for (const auto& m : messages)
				out.push_back({m.status, (int)m.data1, (double)(playhead + m.frameIndex) / kSamplesPerBeat});
		}
		return out;
	}

	std::vector<Event> Only(const std::vector<Event>& events, uint8_t status) {
		std::vector<Event> out;
		for (const auto& e : events) {
			if (e.status == status)
				out.push_back(e);
		}
		return out;
	}

	int CountOf(const std::vector<Event>& events, uint8_t status, int noteNumber) {
		int count = 0;
		for (const auto& e : events) {
			if (e.status == status && e.noteNumber == noteNumber)
				++count;
		}
		return count;
	}

	// the beat an event landed on. one sample of slack: the clip start and the note
	// onset are truncated to samples separately before they are added together
	double BeatOf(const std::vector<Event>& events, uint8_t status, int noteNumber) {
		for (const auto& e : events) {
			if (e.status == status && e.noteNumber == noteNumber)
				return e.beat;
		}
		return -1.0;
	}

	constexpr double kOneSampleInBeats = 1.0 / kSamplesPerBeat;

	// a four-bar run of quarter notes, one pitch per beat, as a clip that plays all of it
	std::shared_ptr<MIDIClip> MakeRun(double startBeat, double duration) {
		auto clip = std::make_shared<MIDIClip>();
		clip->SetStartBeat(startBeat);
		clip->SetDuration(duration);
		for (int i = 0; i < 8; ++i)
			clip->AddNote({60 + i, 100, (double)i, 0.5});
		return clip;
	}

} // namespace

// ================================================================
// THE MAPPING ITSELF
// ================================================================

TEST(ClipTime, AnUncroppedClipPlaysItsMaterialFromItsOwnStart) {
	MIDIClip clip;
	clip.SetStartBeat(8.0);
	clip.SetDuration(4.0);

	ClipTimeMapping mapping = ClipTimeMapping::For(clip);
	EXPECT_DOUBLE_EQ(mapping.contentOrigin, 8.0);
	EXPECT_DOUBLE_EQ(mapping.ToOuterBeat(1.0), 9.0);
	EXPECT_DOUBLE_EQ(mapping.ToContentBeat(9.0), 1.0);
}

TEST(ClipTime, CroppingTheHeadMovesTheWindowAndLeavesTheMaterialWhereItWas) {
	MIDIClip clip;
	clip.SetStartBeat(10.0);
	clip.SetDuration(2.0);
	clip.SetOffset(2.0); // the clip starts two beats into its material

	ClipTimeMapping mapping = ClipTimeMapping::For(clip);
	EXPECT_DOUBLE_EQ(mapping.contentOrigin, 8.0);
	EXPECT_DOUBLE_EQ(mapping.windowStart, 10.0);
	EXPECT_DOUBLE_EQ(mapping.windowEnd, 12.0);
	// the note stored at content beat 2 is the first thing the clip plays
	EXPECT_DOUBLE_EQ(mapping.ToOuterBeat(2.0), 10.0);
}

TEST(ClipTime, MaterialOutsideTheWindowIsCarriedButNotPlayed) {
	MIDIClip clip;
	clip.SetStartBeat(4.0);
	clip.SetDuration(2.0);
	clip.SetOffset(2.0);

	ClipTimeMapping mapping = ClipTimeMapping::For(clip);
	EXPECT_FALSE(mapping.PlaysContentBeat(1.9)); // in front of the window
	EXPECT_TRUE(mapping.PlaysContentBeat(2.0));	// its first beat
	EXPECT_TRUE(mapping.PlaysContentBeat(3.9));
	EXPECT_FALSE(mapping.PlaysContentBeat(4.0)); // one past its last, i.e. the next clip
}

TEST(ClipTime, TheRollsViewOriginJustShiftsTheWholeMapping) {
	MIDIClip clip;
	clip.SetStartBeat(96.0);
	clip.SetDuration(4.0);
	clip.SetOffset(1.0);

	// the piano roll measures everything from the earliest thing on show rather than
	// from bar 1, which is the same mapping with the origin taken out of it
	ClipTimeMapping arrangement = ClipTimeMapping::For(clip);
	ClipTimeMapping view = ClipTimeMapping::For(clip, 95.0);
	EXPECT_DOUBLE_EQ(view.ToOuterBeat(2.5), arrangement.ToOuterBeat(2.5) - 95.0);
	EXPECT_DOUBLE_EQ(view.ToContentBeat(view.ToOuterBeat(2.5)), 2.5);
}

// ================================================================
// AGREEMENT WITH THE SEQUENCER
// ================================================================

TEST(CroppedClip, ANoteIsHeardOnTheBeatTheMappingPutsIt) {
	// the invariant a view has to hold to: whatever beat the mapping draws a note on
	// is the beat the sequencer sounds it at. drawing from the clip start instead put
	// a cropped clip's notes `offset` beats to the right of where they are played
	for (double offset : {0.0, 0.5, 2.0, 3.25}) {
		Track track;
		auto clip = std::make_shared<MIDIClip>();
		clip->SetStartBeat(4.0);
		clip->SetDuration(4.0);
		clip->SetOffset(offset);
		clip->AddNote({60, 100, offset + 1.0, 0.5}); // one beat into the window
		track.AddClip(clip);

		auto events = Render(track, 0.0, 12.0);
		ClipTimeMapping mapping = ClipTimeMapping::For(*clip);
		ASSERT_EQ(CountOf(events, kNoteOn, 60), 1) << "offset " << offset;
		EXPECT_NEAR(BeatOf(events, kNoteOn, 60), mapping.ToOuterBeat(offset + 1.0), kOneSampleInBeats)
			<< "offset " << offset;
	}
}

TEST(CroppedClip, MaterialInFrontOfTheWindowIsNeverSounded) {
	Track track;
	auto clip = MakeRun(4.0, 4.0);
	clip->SetOffset(4.0); // the window opens on the fifth of the eight notes
	track.AddClip(clip);

	auto events = Render(track, 0.0, 12.0);
	for (int i = 0; i < 4; ++i)
		EXPECT_EQ(CountOf(events, kNoteOn, 60 + i), 0) << "note " << i << " is in front of the window";
	for (int i = 4; i < 8; ++i)
		EXPECT_EQ(CountOf(events, kNoteOn, 60 + i), 1) << "note " << i << " is inside the window";
}

TEST(CroppedClip, MaterialPastTheWindowNeitherSoundsNorReleases) {
	// the left half of a split still holds every note the right half plays. gating
	// their release at the clip end used to emit a note-off for a note nothing had
	// started - one stray release per note, at the seam between the two halves
	Track track;
	auto clip = MakeRun(0.0, 4.0); // eight notes, the last four past the end
	track.AddClip(clip);

	auto events = Render(track, 0.0, 12.0);
	for (int i = 4; i < 8; ++i) {
		EXPECT_EQ(CountOf(events, kNoteOn, 60 + i), 0) << "note " << i << " is past the window";
		EXPECT_EQ(CountOf(events, kNoteOff, 60 + i), 0) << "note " << i << " was never started";
	}
}

TEST(CroppedClip, ACutLandingOnANoteOnsetDoesNotBlipOnTheLeftHalf) {
	// splitting exactly where a note begins is the normal case, not a corner one: the
	// cut lands on a grid line and so does the note. the left half must not sound the
	// note that now belongs to the right half, however briefly
	Track track;
	auto clip = std::make_shared<MIDIClip>();
	clip->SetStartBeat(0.0);
	clip->SetDuration(2.0);
	clip->AddNote({60, 100, 0.0, 1.0});
	clip->AddNote({64, 100, 2.0, 1.0}); // starts exactly on the cut
	track.AddClip(clip);

	auto events = Render(track, 0.0, 6.0);
	EXPECT_EQ(CountOf(events, kNoteOn, 64), 0);
	EXPECT_EQ(CountOf(events, kNoteOff, 64), 0);
	EXPECT_EQ(CountOf(events, kNoteOn, 60), 1);
}

TEST(CroppedClip, ANoteRunningPastTheWindowIsStillReleasedAtTheClipEnd) {
	// the counterpart of the test above: a note that DID start inside the window keeps
	// its clamped release, or the instrument would hold it forever
	Track track;
	auto clip = std::make_shared<MIDIClip>();
	clip->SetStartBeat(0.0);
	clip->SetDuration(2.0);
	clip->AddNote({60, 100, 1.0, 8.0}); // starts inside, runs well past the end
	track.AddClip(clip);

	auto events = Render(track, 0.0, 6.0);
	ASSERT_EQ(CountOf(events, kNoteOn, 60), 1);
	ASSERT_EQ(CountOf(events, kNoteOff, 60), 1);
	EXPECT_NEAR(BeatOf(events, kNoteOff, 60), 2.0, 2.0 * kOneSampleInBeats);
}

// ================================================================
// LINKED CLIPS
// ================================================================

TEST(LinkedClips, TheTwoHalvesOfASplitPlayTheSequenceExactlyOnce) {
	// a split hands the halves one shared sequence and two windows onto it. together
	// they have to sound what the whole clip sounded, each note once and on its beat
	Track whole;
	whole.AddClip(MakeRun(0.0, 8.0));
	auto before = Only(Render(whole, 0.0, 12.0), kNoteOn);

	Track split;
	auto left = MakeRun(0.0, 8.0);
	auto right = std::make_shared<MIDIClip>(*left); // the copy shares the sequence
	left->SetDuration(3.0);
	right->SetStartBeat(3.0);
	right->SetDuration(5.0);
	right->SetOffset(3.0);
	split.AddClip(left);
	split.AddClip(right);
	ASSERT_TRUE(left->IsLinkedTo(*right));

	auto after = Only(Render(split, 0.0, 12.0), kNoteOn);

	ASSERT_EQ(after.size(), before.size());
	for (size_t i = 0; i < before.size(); ++i) {
		EXPECT_EQ(after[i].noteNumber, before[i].noteNumber);
		EXPECT_NEAR(after[i].beat, before[i].beat, kOneSampleInBeats);
	}
}

TEST(LinkedClips, CroppingOneWindowLeavesItsSiblingPlayingTheWholeSequence) {
	// two clips over one sequence are two windows, not two copies: trimming the head
	// of one is a change to that window alone
	auto full = MakeRun(0.0, 8.0);
	auto cropped = std::make_shared<MIDIClip>(*full);
	cropped->SetStartBeat(0.0);
	cropped->SetDuration(8.0);
	ASSERT_TRUE(full->IsLinkedTo(*cropped));

	Track croppedTrack;
	croppedTrack.AddClip(cropped);
	cropped->SetStartBeat(4.0);
	cropped->SetDuration(4.0);
	cropped->SetOffset(4.0);

	Track fullTrack;
	fullTrack.AddClip(full);

	EXPECT_EQ(Only(Render(fullTrack, 0.0, 12.0), kNoteOn).size(), 8u);
	EXPECT_EQ(Only(Render(croppedTrack, 0.0, 12.0), kNoteOn).size(), 4u);
	// and the notes themselves were never touched by either window
	EXPECT_EQ(full->GetNotes().size(), 8u);
	EXPECT_EQ(full->GetNotes(), cropped->GetNotes());
}

TEST(LinkedClips, AnOverlapTrimCropsTheHeadOfTheClipItPushesAside) {
	// dropping a clip over the head of another one is the third way a clip ends up
	// cropped, and the one that does it behind the user's back
	Track track;
	auto existing = MakeRun(0.0, 8.0);
	track.AddClip(existing);

	auto dropped = std::make_shared<MIDIClip>();
	dropped->SetStartBeat(0.0);
	dropped->SetDuration(2.0);
	track.AddClip(dropped); // AddClip resolves the overlap

	EXPECT_DOUBLE_EQ(existing->GetStartBeat(), 2.0);
	EXPECT_DOUBLE_EQ(existing->GetDuration(), 6.0);
	EXPECT_DOUBLE_EQ(existing->GetOffset(), 2.0);

	// the trimmed clip keeps sounding its remaining notes where they always were
	auto events = Render(track, 0.0, 12.0);
	ClipTimeMapping mapping = ClipTimeMapping::For(*existing);
	for (int i = 2; i < 8; ++i) {
		ASSERT_EQ(CountOf(events, kNoteOn, 60 + i), 1) << "note " << i;
		EXPECT_NEAR(BeatOf(events, kNoteOn, 60 + i), mapping.ToOuterBeat((double)i), kOneSampleInBeats)
			<< "note " << i;
		EXPECT_NEAR(BeatOf(events, kNoteOn, 60 + i), (double)i, kOneSampleInBeats) << "note " << i;
	}
}
