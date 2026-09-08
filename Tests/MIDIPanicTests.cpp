#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <vector>

#include "AudioProcessor.h"
#include "Clips/MIDIClip.h"
#include "MIDIPanic.h"
#include "Project.h"
#include "Track.h"

// ================================================================
// MIDI PANIC
// ================================================================

// a loop wrap and a seek both flush the instruments and then land on a position that
// usually re-triggers a note at sample 0. the flush is what a track's ledger hands its
// release duty to (Track::AllNotesOff and Track::Reset clear it), so the order the two
// reach the plugin in is the whole of the guarantee: behind the note-on, the release
// lets go of the note that was just started

namespace {

	constexpr int kFrames = 512;
	constexpr int kChannels = 2;
	constexpr double kSampleRate = 48000.0;
	constexpr double kBpm = 120.0;
	constexpr double kSamplesPerBeat = kSampleRate * 60.0 / kBpm;

	constexpr uint8_t kNoteOn = 0x90;
	constexpr uint8_t kNoteOff = 0x80;
	constexpr uint8_t kCC = 0xB0;

	// an instrument voiced straight off the block's MIDI, in the order it arrives -
	// which is what a plugin does, and the only thing that can catch a bad order.
	// the panic is deferred exactly as VST2 and VST3 hosting defer theirs
	class LedgerSynth : public AudioProcessor {
	public:
		const char* GetName() const override { return "Ledger Synth"; }
		std::string GetProcessorId() const override { return "LedgerSynth"; }
		bool IsInstrument() const override { return true; }
		void PrepareToPlay(double) override {}
		void Reset() override { mPendingPanic = true; }
		void AllNotesOff() override { mPendingPanic = true; }

		void Process(float*, int, int, std::vector<MIDIMessage>& mIDIMessages, const ProcessContext&) override {
			if (mPendingPanic) {
				PrependMIDIPanic(mIDIMessages, mActiveNotes, false);
				mPendingPanic = false;
			}
			for (const auto& msg : mIDIMessages) {
				uint8_t type = msg.status & 0xF0;
				if (type == kNoteOn && msg.data2 > 0) {
					mActiveNotes[msg.status & 0x0F].insert(msg.data1);
					mSounding.insert(msg.data1);
				} else if (type == kNoteOff || (type == kNoteOn && msg.data2 == 0)) {
					mActiveNotes[msg.status & 0x0F].erase(msg.data1);
					mSounding.erase(msg.data1);
				} else if (type == kCC && (msg.data1 == 123 || msg.data1 == 120)) {
					mSounding.clear();
				}
			}
		}

		// what the synth has a voice down on, as a plugin would hear it
		std::set<int> mSounding;
	private:
		std::set<int> mActiveNotes[16];
		bool mPendingPanic = false;
	};

	// one track, one instrument, one MIDI clip
	struct Fixture {
		Project project;
		LedgerSynth* synth = nullptr;

		Fixture() {
			project.CreateTrack();
			auto owned = std::make_shared<LedgerSynth>();
			synth = owned.get();
			project.GetTracks()[0]->AddProcessor(owned);
			project.GetTransport().SetSampleRate(kSampleRate);
			project.GetTransport().SetBpm(kBpm);
		}

		std::shared_ptr<MIDIClip> AddClip(double startBeat, double durationBeats) {
			auto clip = std::make_shared<MIDIClip>();
			clip->SetStartBeat(startBeat);
			clip->SetDuration(durationBeats);
			project.GetTracks()[0]->AddClip(clip);
			return clip;
		}

		void Render(int blocks) {
			std::vector<float> buffer(kFrames * kChannels, 0.0f);
			std::vector<MIDIMessage> noLiveMIDI;
			for (int i = 0; i < blocks; ++i) {
				std::fill(buffer.begin(), buffer.end(), 0.0f);
				project.ProcessBlock(buffer.data(), kFrames, kChannels, noLiveMIDI);
			}
		}

		int BlocksFor(double beats) const {
			return (int)(beats * kSamplesPerBeat / kFrames) + 1;
		}
	};

} // namespace

TEST(MIDIPanic, ReleasesEveryHeldNoteAndClearsTheLedger) {
	std::set<int> activeNotes[16];
	activeNotes[0] = {60, 64};
	activeNotes[3] = {48};

	std::vector<MIDIMessage> messages;
	PrependMIDIPanic(messages, activeNotes, false);

	int noteOffs = 0;
	int allNotesOff = 0;
	int allSoundOff = 0;
	for (const auto& msg : messages) {
		if ((msg.status & 0xF0) == kNoteOff)
			++noteOffs;
		else if ((msg.status & 0xF0) == kCC && msg.data1 == 123)
			++allNotesOff;
		else if ((msg.status & 0xF0) == kCC && msg.data1 == 120)
			++allSoundOff;
	}

	EXPECT_EQ(noteOffs, 3);
	EXPECT_EQ(allNotesOff, 16); // one per channel
	EXPECT_EQ(allSoundOff, 0);	// soft panic: reverb/delay tails ring on
	for (const auto& channel : activeNotes)
		EXPECT_TRUE(channel.empty());
}

TEST(MIDIPanic, AHardPanicAlsoSendsAllSoundOff) {
	std::set<int> activeNotes[16];
	std::vector<MIDIMessage> messages;
	PrependMIDIPanic(messages, activeNotes, true);

	int allSoundOff = 0;
	for (const auto& msg : messages) {
		if ((msg.status & 0xF0) == kCC && msg.data1 == 120)
			++allSoundOff;
	}
	EXPECT_EQ(allSoundOff, 16);
}

// the bug this file is named for: appended instead of prepended, the panic's note-offs
// and cc 123 land behind the note-on the wrap or seek just queued and release it
TEST(MIDIPanic, LeadsTheBlockRatherThanTrailingIt) {
	std::set<int> activeNotes[16];
	activeNotes[0] = {62};

	std::vector<MIDIMessage> messages;
	MIDIMessage retrigger;
	retrigger.status = kNoteOn;
	retrigger.data1 = 60;
	retrigger.data2 = 100;
	retrigger.frameIndex = 0;
	messages.push_back(retrigger);

	PrependMIDIPanic(messages, activeNotes, false);

	ASSERT_FALSE(messages.empty());
	EXPECT_EQ(messages.back().status, kNoteOn) << "the block's own events must stay behind the panic";
	EXPECT_EQ(messages.back().data1, 60);
	// and everything ahead of it is the panic, at the head of the block
	for (size_t i = 0; i + 1 < messages.size(); ++i)
		EXPECT_EQ(messages[i].frameIndex, 0);
}

// a note held across the loop end, and the loop end itself off the bar - the report was
// "a loop that ends on a half note gets stuck"
TEST(MIDIPanic, ALoopWrapReleasesWhatItInterruptsAndStillSoundsTheNextPass) {
	Fixture f;
	auto clip = f.AddClip(0.0, 8.0);
	clip->AddNote({60, 100, 0.0, 2.0}); // runs past the loop end below

	auto& transport = f.project.GetTransport();
	transport.SetLoopRange(0, (int64_t)(1.5 * kSamplesPerBeat));
	transport.SetLoopEnabled(true);
	transport.SetPosition(0);
	transport.Play();

	// three passes, so the wrap is exercised more than once
	f.Render(f.BlocksFor(1.5 * 3.0));
	EXPECT_EQ(f.synth->mSounding.count(60), 1u) << "the note fell silent after a wrap";

	transport.Pause();
	f.Render(1);
	EXPECT_TRUE(f.synth->mSounding.empty()) << "a note was left hanging past the stop";
}

// ctrl+space mid-note: PlayFromMarker seeks the running transport back to the marker
TEST(MIDIPanic, ASeekOntoANoteStillSoundsIt) {
	Fixture f;
	auto clip = f.AddClip(0.0, 8.0);
	clip->AddNote({60, 100, 0.0, 4.0});

	auto& transport = f.project.GetTransport();
	transport.SetPosition(0);
	transport.Play();
	f.Render(4); // well inside the note

	transport.SetPosition(0);
	f.Render(3);

	EXPECT_EQ(f.synth->mSounding.count(60), 1u) << "the note the seek landed on is not sounding";
}
