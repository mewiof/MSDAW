#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "Clips/MIDIClip.h"
#include "Project.h"
#include "Track.h"
#include "Views/TimelineView/TimelineClipOps.h"

// ================================================================
// CLIP SELECTION
// ================================================================

// selecting a block of clips comes down to one question: which clips does a box
// drawn over the arrangement cover. the marquee, shift-click and every command that
// works on "the selection" are built on that answer, and it is the only part of the
// feature that can be driven without an ImGui context - the gestures themselves are
// verified in the running app

namespace {

	// tracks named A, B, C, each holding a one-bar clip at bars 1, 2 and 3
	void MakeGrid(Project& project) {
		const char* names[] = {"A", "B", "C"};
		for (int t = 0; t < 3; ++t) {
			project.CreateTrack();
			auto track = project.GetTracks()[t];
			track->SetName(names[t]);
			for (int bar = 0; bar < 3; ++bar) {
				auto clip = std::make_shared<MIDIClip>();
				clip->SetName(std::string(names[t]) + std::to_string(bar));
				clip->SetStartBeat(bar * 4.0);
				clip->SetDuration(4.0);
				track->AddClip(clip);
			}
		}
	}

	std::vector<std::string> NamesOf(const std::vector<std::shared_ptr<Clip>>& clips) {
		std::vector<std::string> names;
		for (const auto& c : clips)
			names.push_back(c->GetName());
		std::sort(names.begin(), names.end());
		return names;
	}

} // namespace

TEST(ClipSelection, ABoxCoversEveryClipInsideItsTrackAndBeatRange) {
	Project project;
	project.Initialize();
	MakeGrid(project);

	// tracks A and B, bars 1 and 2
	auto hits = TimelineClipOps::ClipsInBox(&project, 0, 1, 0.0, 8.0);
	EXPECT_EQ(NamesOf(hits), (std::vector<std::string>{"A0", "A1", "B0", "B1"}));
}

TEST(ClipSelection, AClipOnlyPartlyInsideTheBoxIsStillCaught) {
	Project project;
	project.Initialize();
	MakeGrid(project);

	// a box covering the last beat of bar 1 and the first of bar 2
	auto hits = TimelineClipOps::ClipsInBox(&project, 0, 0, 3.0, 5.0);
	EXPECT_EQ(NamesOf(hits), (std::vector<std::string>{"A0", "A1"}));
}

TEST(ClipSelection, ABoxDraggedStraightDownCatchesTheColumnItPassesThrough) {
	Project project;
	project.Initialize();
	MakeGrid(project);

	// no horizontal travel at all: the box is a vertical line through bar 2
	auto hits = TimelineClipOps::ClipsInBox(&project, 0, 2, 6.0, 6.0);
	EXPECT_EQ(NamesOf(hits), (std::vector<std::string>{"A1", "B1", "C1"}));
}

TEST(ClipSelection, TheBoxCornersMayArriveInEitherOrder) {
	Project project;
	project.Initialize();
	MakeGrid(project);

	// dragging up and to the left has to select the same clips as dragging down right
	auto forward = TimelineClipOps::ClipsInBox(&project, 0, 1, 0.0, 8.0);
	auto reversed = TimelineClipOps::ClipsInBox(&project, 1, 0, 8.0, 0.0);
	EXPECT_EQ(NamesOf(forward), NamesOf(reversed));
}

TEST(ClipSelection, ABoxPastTheLastTrackIsClampedInsteadOfReadingOffTheEnd) {
	Project project;
	project.Initialize();
	MakeGrid(project);

	// the marquee can be dragged into the empty space below the last lane
	auto hits = TimelineClipOps::ClipsInBox(&project, 2, 99, 0.0, 100.0);
	EXPECT_EQ(NamesOf(hits), (std::vector<std::string>{"C0", "C1", "C2"}));
}

TEST(ClipSelection, ABoxOverEmptySpaceCatchesNothing) {
	Project project;
	project.Initialize();
	MakeGrid(project);

	// past the end of every clip
	auto hits = TimelineClipOps::ClipsInBox(&project, 0, 2, 40.0, 48.0);
	EXPECT_TRUE(hits.empty());
}

TEST(ClipSelection, HitsComeBackInTrackThenTimeOrder) {
	Project project;
	project.Initialize();
	MakeGrid(project);

	// a track holds its clips in insertion order, so the ordering has to be imposed:
	// the duplicate/copy commands read the block's first clip to place the rest
	auto track = project.GetTracks()[0];
	auto late = std::make_shared<MIDIClip>();
	late->SetName("A-late");
	late->SetStartBeat(-2.0); // before everything else, added last
	late->SetDuration(1.0);
	track->SetClips([&] {
		auto clips = track->GetClips();
		clips.push_back(late);
		return clips;
	}());

	auto hits = TimelineClipOps::ClipsInBox(&project, 0, 1, -4.0, 8.0);
	ASSERT_GE(hits.size(), 2u);
	EXPECT_EQ(hits.front()->GetName(), "A-late");
	EXPECT_EQ(hits.back()->GetName(), "B1");
}

TEST(ClipSelection, TheOwningTrackOfAClipIsFoundByIdentity) {
	Project project;
	project.Initialize();
	MakeGrid(project);

	auto clip = project.GetTracks()[1]->GetClips()[2];
	EXPECT_EQ(TimelineClipOps::FindTrackIndex(&project, clip), 1);

	// a clip that has left the arrangement reports no track, which is what the
	// selection prune leans on
	auto detached = std::make_shared<MIDIClip>();
	EXPECT_EQ(TimelineClipOps::FindTrackIndex(&project, detached), -1);
}
