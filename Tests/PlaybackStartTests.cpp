#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "Clips/MIDIClip.h"
#include "Project.h"
#include "Track.h"

// ================================================================
// STARTING PLAYBACK
// ================================================================

// Project::ProcessBlock spots a discontinuous playhead and panics the graph, so nothing
// is left ringing from the old position. that panic is delivered at the head of the next
// block, in front of whatever the sequencer put in it - which is fine for a real seek and
// ruinous for the block that merely starts playback: a note sitting exactly on the play
// position was released in the same buffer it was struck in. these pin down that starting
// from a stop is not a seek, while a jump made while already running still is

namespace {

	constexpr int kNumFrames = 128;
	constexpr int kNumChannels = 2;
	constexpr double kSampleRate = 48000.0;
	constexpr double kBpm = 120.0;
	constexpr double kSamplesPerBeat = kSampleRate * 60.0 / kBpm;

	// an instrument in name only: it counts the panics it is asked for and keeps the
	// MIDI of the last block it saw, which is all either assertion needs
	class ProbeInstrument : public AudioProcessor {
	public:
		int resets = 0;
		int allNotesOffs = 0;
		std::vector<MIDIMessage> lastBlock;

		const char* GetName() const override { return "Probe"; }
		bool IsInstrument() const override { return true; }
		void PrepareToPlay(double) override {}
		void Reset() override { ++resets; }
		void AllNotesOff() override { ++allNotesOffs; }
		void Process(float*, int, int, std::vector<MIDIMessage>& mIDIMessages, const ProcessContext&) override {
			lastBlock = mIDIMessages;
		}
	};

	// one track carrying the probe and a one-beat note at `startBeat`
	struct Fixture {
		Project project;
		std::shared_ptr<ProbeInstrument> probe = std::make_shared<ProbeInstrument>();
		std::vector<float> buffer = std::vector<float>(kNumFrames * kNumChannels, 0.0f);

		explicit Fixture(double startBeat) {
			project.Initialize();
			project.CreateTrack();
			auto& track = project.GetTracks()[0];
			track->AddProcessor(probe);

			auto clip = std::make_shared<MIDIClip>();
			clip->SetStartBeat(startBeat);
			clip->SetDuration(1.0);
			clip->AddNote({60, 100, 0.0, 1.0});
			track->AddClip(clip);
		}

		void RenderBlock() {
			std::vector<MIDIMessage> live;
			project.ProcessBlock(buffer.data(), kNumFrames, kNumChannels, live);
		}

		void SeekToBeat(double beat) {
			project.GetTransport().SetPosition((int64_t)(beat * kSamplesPerBeat));
		}
	};

	bool HasNoteOn(const std::vector<MIDIMessage>& messages, int noteNumber) {
		for (const auto& m : messages) {
			if (m.status == 0x90 && m.data1 == (uint8_t)noteNumber && m.data2 > 0)
				return true;
		}
		return false;
	}

} // namespace

// the bug: a stopped transport does not advance, so the block that started playback never
// continued the previous one and read as a seek every time. the panic that followed landed
// in front of the note the sequencer struck on that very sample, and the instrument let go
// of it before it sounded - the clip appeared to swallow its first note
TEST(PlaybackStart, StartingFromAStopIsNotASeek) {
	Fixture f(8.0);

	f.RenderBlock(); // stopped: the transport sits still while the callback keeps running
	f.RenderBlock();
	EXPECT_EQ(f.probe->resets, 0);

	f.SeekToBeat(8.0);
	f.project.GetTransport().Play();
	f.RenderBlock();

	EXPECT_EQ(f.probe->resets, 0);
	EXPECT_TRUE(HasNoteOn(f.probe->lastBlock, 60));
}

// a note landing on the play position is the whole point of putting the playhead there,
// and it has to arrive at the head of that first block rather than a block late
TEST(PlaybackStart, ANoteOnThePlayPositionFiresInTheFirstBlock) {
	Fixture f(8.0);

	f.SeekToBeat(8.0);
	f.project.GetTransport().Play();
	f.RenderBlock();

	ASSERT_TRUE(HasNoteOn(f.probe->lastBlock, 60));
	EXPECT_EQ(f.probe->lastBlock.front().frameIndex, 0);
}

// the other half of the contract: a jump made while the transport is already running is
// still a seek, and still resets the graph
TEST(PlaybackStart, AJumpWhileRunningStillResetsTheGraph) {
	Fixture f(8.0);

	f.project.GetTransport().Play();
	f.RenderBlock();
	f.RenderBlock();
	ASSERT_EQ(f.probe->resets, 0); // contiguous playback resets nothing

	f.SeekToBeat(8.0);
	f.RenderBlock();

	EXPECT_EQ(f.probe->resets, 1);
}

// stopping still panics: that is what leaves nothing sounding for the next start to
// have to clean up, and is why skipping the reset on start is safe
TEST(PlaybackStart, StoppingStillResetsTheGraph) {
	Fixture f(8.0);

	f.project.GetTransport().Play();
	f.RenderBlock();
	f.project.GetTransport().Pause();
	f.RenderBlock();

	EXPECT_EQ(f.probe->resets, 1);
}
