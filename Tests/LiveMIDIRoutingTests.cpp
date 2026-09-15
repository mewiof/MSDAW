#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "Project.h"
#include "Track.h"

// ================================================================
// LIVE MIDI ROUTING
// ================================================================

// notes played on the computer keyboard are handed to the graph as a plain list, once
// per block, and the graph decides which instrument gets them. it used to decide that
// per event against whatever track was selected at the time, which is fine for a note-on
// and wrong for the release: a key held while the selection moved (or while its track was
// muted) had its note-off delivered to a different instrument, and the one actually
// holding the note was never addressed again. these pin the release to the note

namespace {

	constexpr int kNumFrames = 64;
	constexpr int kNumChannels = 2;

	// stands in for a synth: records the MIDI it is handed, block by block
	class ProbeInstrument : public AudioProcessor {
	public:
		std::vector<MIDIMessage> received;

		const char* GetName() const override { return "Probe"; }
		bool IsInstrument() const override { return true; }
		void PrepareToPlay(double) override {}
		void Process(float*, int, int, std::vector<MIDIMessage>& mIDIMessages, const ProcessContext&) override {
			received.insert(received.end(), mIDIMessages.begin(), mIDIMessages.end());
		}
	};

	// two instrument tracks, so a note can be started on one and released after the
	// selection has moved to the other
	struct Fixture {
		Project project;
		std::shared_ptr<ProbeInstrument> probeA = std::make_shared<ProbeInstrument>();
		std::shared_ptr<ProbeInstrument> probeB = std::make_shared<ProbeInstrument>();
		std::vector<float> buffer = std::vector<float>(kNumFrames * kNumChannels, 0.0f);

		Fixture() {
			project.Initialize();
			project.CreateTrack();
			project.CreateTrack();
			project.GetTracks()[0]->AddProcessor(probeA);
			project.GetTracks()[1]->AddProcessor(probeB);
		}

		void RenderBlock(std::vector<MIDIMessage> live = {}) {
			project.ProcessBlock(buffer.data(), kNumFrames, kNumChannels, live);
		}
	};

	MIDIMessage NoteOn(int noteNumber) {
		MIDIMessage m;
		m.status = 0x90;
		m.data1 = (uint8_t)noteNumber;
		m.data2 = 100;
		m.frameIndex = 0;
		return m;
	}

	MIDIMessage NoteOff(int noteNumber) {
		MIDIMessage m;
		m.status = 0x80;
		m.data1 = (uint8_t)noteNumber;
		m.data2 = 0;
		m.frameIndex = 0;
		return m;
	}

	int CountOf(const std::vector<MIDIMessage>& messages, uint8_t status, int noteNumber) {
		int count = 0;
		for (const auto& m : messages) {
			if (m.status == status && m.data1 == (uint8_t)noteNumber)
				++count;
		}
		return count;
	}

} // namespace

// the plain case still works: the selected instrument gets both halves
TEST(LiveMIDIRouting, TheSelectedInstrumentGetsWhatIsPlayed) {
	Fixture f;
	f.project.SetSelectedTrack(0);

	f.RenderBlock({NoteOn(60)});
	f.RenderBlock({NoteOff(60)});

	EXPECT_EQ(CountOf(f.probeA->received, 0x90, 60), 1);
	EXPECT_EQ(CountOf(f.probeA->received, 0x80, 60), 1);
	EXPECT_TRUE(f.probeB->received.empty());
}

// the bug: hold a key, click another track, let go. the release went to the track that
// was now selected and the first instrument held the note forever
TEST(LiveMIDIRouting, AReleaseFollowsTheNoteWhenTheSelectionMoves) {
	Fixture f;
	f.project.SetSelectedTrack(0);
	f.RenderBlock({NoteOn(60)});

	f.project.SetSelectedTrack(1);
	f.RenderBlock({NoteOff(60)});

	EXPECT_EQ(CountOf(f.probeA->received, 0x80, 60), 1); // released where it is sounding
	EXPECT_EQ(CountOf(f.probeB->received, 0x80, 60), 0); // and not on the newly selected one
}

// muting the track under a held key used to skip it entirely, so the release never
// arrived and the note came back the moment the track was unmuted
TEST(LiveMIDIRouting, AMutedTrackStillGetsTheReleaseItIsOwed) {
	Fixture f;
	f.project.SetSelectedTrack(0);
	f.RenderBlock({NoteOn(60)});

	f.project.GetTracks()[0]->SetMute(true);
	f.RenderBlock({NoteOff(60)});

	EXPECT_EQ(CountOf(f.probeA->received, 0x80, 60), 1);
}

// re-striking the same note on another track without releasing it first: the old one
// has to be let go of, or nothing addresses it again
TEST(LiveMIDIRouting, RestrikingOnAnotherTrackReleasesTheFirst) {
	Fixture f;
	f.project.SetSelectedTrack(0);
	f.RenderBlock({NoteOn(60)});

	f.project.SetSelectedTrack(1);
	f.RenderBlock({NoteOn(60)});

	EXPECT_EQ(CountOf(f.probeA->received, 0x80, 60), 1);
	EXPECT_EQ(CountOf(f.probeB->received, 0x90, 60), 1);
}

// a track with no instrument on it cannot sound a note, and must not swallow one either
TEST(LiveMIDIRouting, ATrackWithNoInstrumentIsNotGivenNotes) {
	Fixture f;
	f.project.CreateTrack(); // third track, empty
	f.project.SetSelectedTrack(2);

	f.RenderBlock({NoteOn(60)});
	f.RenderBlock({NoteOff(60)});

	EXPECT_TRUE(f.probeA->received.empty());
	EXPECT_TRUE(f.probeB->received.empty());
}
