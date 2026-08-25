#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <vector>

#include "Clips/AudioClip.h"
#include "Project.h"
#include "Track.h"

// ================================================================
// SOLO / MUTE ROUTING
// ================================================================

// solo and mute interact per track and across a group hierarchy, and the rules
// only show up in the rendered sum, so these drive a real block through
// Project::ProcessBlock and compare what came out

namespace {

	constexpr int kFrames = 256;
	constexpr int kChannels = 2;

	// an audible track: one audio clip covering the block, from the very first sample
	void GiveTrackAudio(const std::shared_ptr<Track>& track) {
		auto clip = std::make_shared<AudioClip>();
		clip->GenerateTestSignal(48000.0, 1.0);
		clip->SetStartBeat(0.0);
		clip->SetDuration(2.0);
		track->AddClip(clip);
	}

	std::vector<float> RenderOneBlock(Project& project) {
		// rewind for every render so two takes are sample-aligned and comparable
		project.GetTransport().SetPosition(0);
		project.GetTransport().Play();

		std::vector<float> buffer(kFrames * kChannels, 0.0f);
		std::vector<MIDIMessage> noLiveMIDI;
		project.ProcessBlock(buffer.data(), kFrames, kChannels, noLiveMIDI);

		project.GetTransport().Pause();
		return buffer;
	}

	float Peak(const std::vector<float>& buffer) {
		float peak = 0.0f;
		for (float sample : buffer)
			peak = std::max(peak, std::abs(sample));
		return peak;
	}

	// two audible tracks wrapped in a group. returns the group's index
	int MakeGroupOfTwo(Project& project) {
		project.CreateTrack();
		project.CreateTrack();
		GiveTrackAudio(project.GetTracks()[0]);
		GiveTrackAudio(project.GetTracks()[1]);
		project.GroupSelectedTracks({0, 1});

		for (int i = 0; i < (int)project.GetTracks().size(); ++i) {
			if (project.GetTracks()[i]->IsGroup())
				return i;
		}
		return -1;
	}

} // namespace

TEST(SoloMute, GroupingProducesAGroupWithTwoAudibleChildren) {
	Project project;
	project.Initialize();
	int group = MakeGroupOfTwo(project);

	ASSERT_GE(group, 0);
	ASSERT_EQ(project.GetTracks().size(), 3u);
	EXPECT_GT(Peak(RenderOneBlock(project)), 0.0f);
}

// the bug: soloing a group made ancestor-solo count as "soloed", which let it
// bypass a child's own mute and play a track the user had silenced
TEST(SoloMute, SoloingAGroupStillRespectsAMutedChild) {
	Project project;
	project.Initialize();
	int group = MakeGroupOfTwo(project);
	ASSERT_GE(group, 0);

	auto& tracks = project.GetTracks();
	std::shared_ptr<Track> mutedChild = nullptr;
	for (const auto& t : tracks) {
		if (!t->IsGroup()) {
			mutedChild = t;
			break;
		}
	}
	ASSERT_NE(mutedChild, nullptr);

	mutedChild->SetMute(true);
	std::vector<float> mutedOnly = RenderOneBlock(project);

	tracks[group]->SetSolo(true);
	std::vector<float> mutedAndSoloedGroup = RenderOneBlock(project);

	EXPECT_EQ(mutedAndSoloedGroup, mutedOnly) << "soloing the group must not un-mute a child";

	// control: the muted child really does contribute when it is not muted, so the
	// comparison above is not two silent buffers agreeing with each other
	mutedChild->SetMute(false);
	std::vector<float> nothingMuted = RenderOneBlock(project);
	EXPECT_NE(nothingMuted, mutedOnly);
}

// a track's own solo is still the one thing that overrides its own mute
TEST(SoloMute, ATrackSoloedItselfOverridesItsOwnMute) {
	Project project;
	project.Initialize();
	project.CreateTrack();
	GiveTrackAudio(project.GetTracks()[0]);

	auto track = project.GetTracks()[0];
	track->SetMute(true);
	EXPECT_FLOAT_EQ(Peak(RenderOneBlock(project)), 0.0f);

	track->SetSolo(true);
	EXPECT_GT(Peak(RenderOneBlock(project)), 0.0f);
}

// solo elsewhere silences a track that is neither soloed nor under a soloed group
TEST(SoloMute, SoloOnOneTrackSilencesTheOthers) {
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.CreateTrack();
	GiveTrackAudio(project.GetTracks()[0]);
	GiveTrackAudio(project.GetTracks()[1]);

	std::vector<float> both = RenderOneBlock(project);

	project.GetTracks()[0]->SetSolo(true);
	std::vector<float> onlyFirst = RenderOneBlock(project);

	EXPECT_GT(Peak(onlyFirst), 0.0f);
	EXPECT_NE(onlyFirst, both);
}
