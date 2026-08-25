#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <set>

#include "Project.h"
#include "Track.h"

// ================================================================
// TRACK TOPOLOGY
// ================================================================

// mTracks is flat and a group's children follow their header, so where a new track
// lands and what it calls its parent are two answers to the same question. the track
// list's context menus are the only callers, and neither can be driven headlessly

namespace {

	std::vector<std::string> Names(Project& project) {
		std::vector<std::string> names;
		for (const auto& track : project.GetTracks())
			names.push_back(track->GetName());
		return names;
	}

	int IndexOf(Project& project, const std::string& name) {
		const auto& tracks = project.GetTracks();
		for (int i = 0; i < (int)tracks.size(); ++i) {
			if (tracks[i]->GetName() == name)
				return i;
		}
		return -1;
	}

	// three root tracks named A, B and C
	void MakeThreeTracks(Project& project) {
		for (int i = 0; i < 3; ++i)
			project.CreateTrack();
		project.GetTracks()[0]->SetName("A");
		project.GetTracks()[1]->SetName("B");
		project.GetTracks()[2]->SetName("C");
	}

} // namespace

TEST(TrackTopology, CreateTrackAppendsAtTheRoot) {
	Project project;
	project.Initialize();
	MakeThreeTracks(project);

	project.CreateTrack();

	ASSERT_EQ(project.GetTracks().size(), 4u);
	EXPECT_EQ(project.GetTracks()[3]->GetParent(), nullptr);
	EXPECT_EQ(Names(project)[2], "C") << "the existing order must not move";
}

TEST(TrackTopology, CreateTrackAfterLandsDirectlyBelowThatTrack) {
	Project project;
	project.Initialize();
	MakeThreeTracks(project);

	project.CreateTrackAfter(0);

	ASSERT_EQ(project.GetTracks().size(), 4u);
	EXPECT_EQ(Names(project)[0], "A");
	EXPECT_EQ(Names(project)[2], "B") << "the new track goes between A and B";
	EXPECT_EQ(Names(project)[3], "C");
	EXPECT_EQ(project.GetTracks()[1]->GetParent(), nullptr);
}

TEST(TrackTopology, CreateTrackAfterTheLastTrackAppends) {
	Project project;
	project.Initialize();
	MakeThreeTracks(project);

	project.CreateTrackAfter(2);

	ASSERT_EQ(project.GetTracks().size(), 4u);
	EXPECT_EQ(Names(project)[2], "C");
	EXPECT_EQ(project.GetTracks()[3]->GetParent(), nullptr);
}

// an index that no longer exists (a stale menu, a track deleted underneath) must not
// index off the end
TEST(TrackTopology, CreateTrackAfterAnUnknownIndexAppendsAtTheRoot) {
	Project project;
	project.Initialize();
	MakeThreeTracks(project);

	project.CreateTrackAfter(99);
	project.CreateTrackAfter(-1);

	ASSERT_EQ(project.GetTracks().size(), 5u);
	EXPECT_EQ(project.GetTracks()[3]->GetParent(), nullptr);
	EXPECT_EQ(project.GetTracks()[4]->GetParent(), nullptr);
}

TEST(TrackTopology, ATrackAddedBelowOneInAGroupJoinsThatGroup) {
	Project project;
	project.Initialize();
	MakeThreeTracks(project);
	project.GroupSelectedTracks({0, 1}); // A and B under a new group header

	const int indexOfA = IndexOf(project, "A");
	ASSERT_GT(indexOfA, 0);
	auto group = project.GetTracks()[indexOfA]->GetParent();
	ASSERT_NE(group, nullptr);

	project.CreateTrackAfter(indexOfA);

	auto added = project.GetTracks()[indexOfA + 1];
	EXPECT_EQ(added->GetParent(), group) << "a sibling of A belongs to A's group";
	EXPECT_EQ(Names(project)[indexOfA + 2], "B") << "and sits between A and B";
}

// the case that decides what "after this one" means: a group header owns everything
// below it, so the new track goes after the whole group rather than becoming its
// first child
TEST(TrackTopology, ATrackAddedBelowAGroupHeaderClearsTheWholeGroup) {
	Project project;
	project.Initialize();
	MakeThreeTracks(project);
	project.GroupSelectedTracks({0, 1});

	const int headerIndex = IndexOf(project, "Group");
	ASSERT_GE(headerIndex, 0);
	ASSERT_TRUE(project.GetTracks()[headerIndex]->IsGroup());

	project.CreateTrackAfter(headerIndex);

	// header, A, B, then the new one - not header, new one, A, B
	ASSERT_EQ(project.GetTracks().size(), 5u);
	EXPECT_EQ(Names(project)[headerIndex + 1], "A");
	EXPECT_EQ(Names(project)[headerIndex + 2], "B");
	EXPECT_EQ(project.GetTracks()[headerIndex + 3]->GetParent(), nullptr)
		<< "the new track is a sibling of the group, not one of its children";
}

TEST(TrackTopology, ANestedGroupIsSteppedOverWholesale) {
	Project project;
	project.Initialize();
	MakeThreeTracks(project);
	project.GroupSelectedTracks({0, 1});	// inner group over A and B
	project.GroupSelectedTracks({0, 1, 2}); // outer group over the inner one and its children

	const int outerIndex = 0;
	ASSERT_TRUE(project.GetTracks()[outerIndex]->IsGroup());
	const size_t before = project.GetTracks().size();

	project.CreateTrackAfter(outerIndex);

	ASSERT_EQ(project.GetTracks().size(), before + 1);
	EXPECT_EQ(project.GetTracks().back()->GetParent(), nullptr);
	EXPECT_EQ(Names(project).back(), project.GetTracks()[before]->GetName())
		<< "the new track clears every descendant of the outer group";
}

// a group is a mixing container: the timeline refuses to place clips on one, and
// every drop site asks the track itself rather than re-deriving the rule
TEST(TrackTopology, AGroupDoesNotAcceptClips) {
	Project project;
	project.Initialize();
	MakeThreeTracks(project);
	project.GroupSelectedTracks({0, 1});

	int groups = 0;
	for (const auto& track : project.GetTracks()) {
		if (track->IsGroup()) {
			++groups;
			EXPECT_FALSE(track->AcceptsClips());
		} else {
			EXPECT_TRUE(track->AcceptsClips());
		}
	}
	EXPECT_EQ(groups, 1);
}

// an open automation lane owns the row while it is up, so it takes no clips either
TEST(TrackTopology, ATrackShowingItsAutomationLaneDoesNotAcceptClips) {
	Project project;
	project.Initialize();
	project.CreateTrack();

	auto track = project.GetTracks()[0];
	ASSERT_TRUE(track->AcceptsClips());

	track->mShowAutomation = true;
	EXPECT_FALSE(track->AcceptsClips());
}
