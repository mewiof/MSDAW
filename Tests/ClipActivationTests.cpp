#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include "Project.h"
#include "Track.h"
#include "Undo/Actions.h"

// ================================================================
// CLIP ACTIVATION
// ================================================================

// a deactivated clip keeps its place on the timeline and is skipped by the
// sequencer. the flag is read on the audio thread, so what it does to a rendered
// block is the part worth pinning down; the timeline visuals are not testable here

namespace {

	ProcessContext PlayingContext() {
		ProcessContext context;
		context.sampleRate = 48000.0;
		context.bpm = 120.0;
		context.currentSample = 0;
		context.isPlaying = true;
		return context;
	}

	// a clip holding one note that starts on the clip's first beat
	std::shared_ptr<MIDIClip> ClipWithOneNote() {
		auto clip = std::make_shared<MIDIClip>();
		clip->SetStartBeat(0.0);
		clip->SetDuration(4.0);
		clip->AddNote({60, 100, 0.0, 1.0});
		return clip;
	}

	bool BufferIsSilent(const std::vector<float>& buffer) {
		for (float sample : buffer) {
			if (sample != 0.0f)
				return false;
		}
		return true;
	}

} // namespace

TEST(ClipActivation, ClipsStartOutActive) {
	MIDIClip clip;

	EXPECT_TRUE(clip.IsEnabled());
}

TEST(ClipActivation, AnActiveMIDIClipEmitsItsNotes) {
	Track track;
	track.AddClip(ClipWithOneNote());

	std::vector<float> buffer(512 * 2, 0.0f);
	std::vector<MIDIMessage> messages;
	track.Process(buffer.data(), 512, 2, messages, PlayingContext());

	ASSERT_FALSE(messages.empty());
	EXPECT_EQ(messages[0].status, 0x90);
	EXPECT_EQ(messages[0].data1, 60);
}

TEST(ClipActivation, ADeactivatedMIDIClipEmitsNothing) {
	Track track;
	auto clip = ClipWithOneNote();
	clip->SetEnabled(false);
	track.AddClip(clip);

	std::vector<float> buffer(512 * 2, 0.0f);
	std::vector<MIDIMessage> messages;
	track.Process(buffer.data(), 512, 2, messages, PlayingContext());

	EXPECT_TRUE(messages.empty());
}

TEST(ClipActivation, ADeactivatedAudioClipRendersSilence) {
	auto clip = std::make_shared<AudioClip>();
	clip->GenerateTestSignal(48000.0, 1.0);
	clip->SetStartBeat(0.0);
	clip->SetDuration(2.0);

	Track track;
	track.AddClip(clip);

	std::vector<float> buffer(512 * 2, 0.0f);
	std::vector<MIDIMessage> messages;
	track.Process(buffer.data(), 512, 2, messages, PlayingContext());
	ASSERT_FALSE(BufferIsSilent(buffer)) << "the clip must be audible before it is deactivated";

	clip->SetEnabled(false);
	std::fill(buffer.begin(), buffer.end(), 0.0f);
	track.Process(buffer.data(), 512, 2, messages, PlayingContext());

	EXPECT_TRUE(BufferIsSilent(buffer));
}

// a deactivated clip is skipped, not moved: neighbours must not slide into its span
TEST(ClipActivation, DeactivationLeavesTheClipInPlace) {
	Track track;
	auto clip = ClipWithOneNote();
	track.AddClip(clip);

	clip->SetEnabled(false);

	ASSERT_EQ(track.GetClips().size(), 1u);
	EXPECT_DOUBLE_EQ(clip->GetStartBeat(), 0.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 4.0);
}

TEST(ClipActivation, ToggleIsOneUndoStep) {
	Project project;
	project.Initialize();
	project.CreateTrack();

	auto track = project.GetTracks()[0];
	auto clip = ClipWithOneNote();
	track->AddClip(clip);

	UndoManager undoManager;
	ToggleClipEnabled(&project, undoManager, track, clip);
	EXPECT_FALSE(clip->IsEnabled());
	ASSERT_TRUE(undoManager.CanUndo());
	EXPECT_STREQ(undoManager.PeekUndoName(), "Deactivate clip");

	undoManager.Undo();
	EXPECT_TRUE(clip->IsEnabled());
	EXPECT_EQ(track->GetClips().size(), 1u) << "undo must not drop the clip from the track";

	undoManager.Redo();
	EXPECT_FALSE(clip->IsEnabled());
}
