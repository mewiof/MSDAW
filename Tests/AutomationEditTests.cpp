#include "AutomationEdits.h"
#include <gtest/gtest.h>

using namespace AutomationEdits;

namespace {

// the shape a dip is drawn as: a point at the top, a second one a hair later at the
// bottom (so the fall reads as vertical), then a curved climb back to the top
std::vector<AutomationPoint> MakeDip() {
	return {
		{2.0, 1.0f, 0.0f, true},
		{2.02, 0.0f, 0.7f, true},
		{3.0, 1.0f, 0.0f, true},
	};
}

void SelectAll(std::vector<AutomationPoint>& points, bool selected = true) {
	for (auto& p : points)
		p.selected = selected;
}

// the contract: after a duplicate, the stretch starting one block-length later has to hold
// the same points, in the same order, with the same values and the same tensions
void ExpectReplica(const std::vector<AutomationPoint>& points, const std::vector<AutomationPoint>& block) {
	ASSERT_FALSE(block.empty());
	double length = block.back().beat - block.front().beat;

	for (size_t i = 0; i < block.size(); ++i) {
		double wantBeat = block[i].beat + length;
		int found = IndexAtBeat(points, wantBeat);
		ASSERT_NE(found, -1) << "no point at beat " << wantBeat;
		EXPECT_FLOAT_EQ(points[found].value, block[i].value) << "value at beat " << wantBeat;
		EXPECT_FLOAT_EQ(points[found].tension, block[i].tension) << "tension at beat " << wantBeat;
		EXPECT_TRUE(points[found].selected) << "copy not selected at beat " << wantBeat;
	}
}

} // namespace

TEST(AutomationEdits, DuplicateReproducesTheBlockExactlyOneBlockLengthLater) {
	auto block = MakeDip();
	auto points = block;
	DuplicateSelection(points, 0.25);

	ExpectReplica(points, block);

	// the block's last point and the copy's first want the same beat, so a three point dip
	// comes out as five, not six
	ASSERT_EQ(points.size(), 5u);
	EXPECT_DOUBLE_EQ(points[0].beat, 2.0);
	EXPECT_NEAR(points[1].beat, 2.02, kBeatEpsilon);
	EXPECT_DOUBLE_EQ(points[2].beat, 3.0);
	EXPECT_NEAR(points[3].beat, 3.02, kBeatEpsilon);
	EXPECT_DOUBLE_EQ(points[4].beat, 4.0);
}

TEST(AutomationEdits, DuplicateKeepsEveryTensionIncludingTheLeadingOne) {
	// every point carries a different tension, so a dropped or defaulted one shows up
	std::vector<AutomationPoint> block = {
		{0.0, 0.2f, -0.6f, true},
		{1.0, 0.9f, 0.35f, true},
		{2.5, 0.4f, -0.15f, true},
		{4.0, 0.2f, 0.8f, true},
	};
	auto points = block;
	DuplicateSelection(points, 0.25);

	ExpectReplica(points, block);
	ASSERT_EQ(points.size(), 7u);
}

TEST(AutomationEdits, DuplicateChainsBecauseTheCopyBecomesTheSelection) {
	auto block = MakeDip();
	auto points = block;
	DuplicateSelection(points, 0.25);
	DuplicateSelection(points, 0.25);

	ASSERT_EQ(points.size(), 7u);
	EXPECT_DOUBLE_EQ(points[4].beat, 4.0);
	EXPECT_NEAR(points[5].beat, 4.02, kBeatEpsilon);
	EXPECT_DOUBLE_EQ(points[6].beat, 5.0);

	// the second repeat traces the first
	EXPECT_FLOAT_EQ(points[5].value, points[3].value);
	EXPECT_FLOAT_EQ(points[5].tension, points[3].tension);
}

TEST(AutomationEdits, DuplicateStepsByTheBlockEvenWhenItIsOffGrid) {
	// an off-grid block still repeats flush against itself -- no rounding to a division
	std::vector<AutomationPoint> block = {
		{2.05, 1.0f, 0.0f, true},
		{3.35, 0.0f, 0.5f, true},
	};
	auto points = block;
	DuplicateSelection(points, 0.25);

	ExpectReplica(points, block);
	ASSERT_EQ(points.size(), 3u);
	EXPECT_NEAR(points[2].beat, 4.65, kBeatEpsilon);
}

TEST(AutomationEdits, DuplicateStepsASinglePointByOneGridDivision) {
	std::vector<AutomationPoint> points = {{1.0, 0.5f, 0.3f, true}};
	DuplicateSelection(points, 0.25);

	ASSERT_EQ(points.size(), 2u);
	EXPECT_DOUBLE_EQ(points[1].beat, 1.25);
	EXPECT_FLOAT_EQ(points[1].tension, 0.3f);
}

TEST(AutomationEdits, DuplicateIgnoresUnselectedPoints) {
	auto points = MakeDip();
	SelectAll(points, false);
	DuplicateSelection(points, 0.25);

	EXPECT_EQ(points.size(), 3u);
}

TEST(AutomationEdits, DuplicateLeavesWhatSitsOutsideTheBlockAlone) {
	auto block = MakeDip();
	auto points = block;
	points.push_back({1.0, 0.5f, 0.0f, false});  // before the block
	points.push_back({3.5, 0.25f, 0.0f, false}); // inside the repeat
	points.push_back({6.0, 0.5f, 0.0f, false});  // past the repeat
	SortByBeat(points);
	DuplicateSelection(points, 0.25);

	ExpectReplica(points, block);
	// only the point the repeat lands on is displaced
	EXPECT_NE(IndexAtBeat(points, 1.0), -1);
	EXPECT_EQ(IndexAtBeat(points, 3.5), -1);
	EXPECT_NE(IndexAtBeat(points, 6.0), -1);
}

TEST(AutomationEdits, CopyRebasesToTheEarliestSelectedPoint) {
	auto points = MakeDip();
	auto clipboard = CopySelection(points);

	ASSERT_EQ(clipboard.size(), 3u);
	EXPECT_DOUBLE_EQ(clipboard[0].beat, 0.0);
	// rebasing subtracts, so the offsets carry the usual double residue
	EXPECT_NEAR(clipboard[1].beat, 0.02, kBeatEpsilon);
	EXPECT_NEAR(clipboard[2].beat, 1.0, kBeatEpsilon);
	EXPECT_FLOAT_EQ(clipboard[1].tension, 0.7f);
}

TEST(AutomationEdits, PasteLandsOnTheAnchorAndKeepsTension) {
	auto source = MakeDip();
	auto clipboard = CopySelection(source);

	std::vector<AutomationPoint> points;
	PasteAt(points, clipboard, 8.0, 0.0f, 1.0f);

	ASSERT_EQ(points.size(), 3u);
	EXPECT_DOUBLE_EQ(points[0].beat, 8.0);
	EXPECT_DOUBLE_EQ(points[2].beat, 9.0);
	EXPECT_FLOAT_EQ(points[1].tension, 0.7f);
	EXPECT_TRUE(points[0].selected);
}

TEST(AutomationEdits, PasteReplacesTheSpanItLandsOn) {
	auto source = MakeDip();
	auto clipboard = CopySelection(source);

	std::vector<AutomationPoint> points = {
		{7.5, 0.1f, 0.0f, false},
		{8.5, 0.2f, 0.0f, false},
		{9.5, 0.3f, 0.0f, false},
	};
	PasteAt(points, clipboard, 8.0, 0.0f, 1.0f);

	// only the point inside the pasted span is displaced
	ASSERT_EQ(points.size(), 5u);
	EXPECT_EQ(IndexAtBeat(points, 8.5), -1);
	EXPECT_NE(IndexAtBeat(points, 7.5), -1);
	EXPECT_NE(IndexAtBeat(points, 9.5), -1);
}

TEST(AutomationEdits, PasteClampsToTheParameterRange) {
	std::vector<AutomationPoint> clipboard = {{0.0, 5.0f, 0.0f, false}};
	std::vector<AutomationPoint> points;
	PasteAt(points, clipboard, 0.0, 0.0f, 1.0f);

	ASSERT_EQ(points.size(), 1u);
	EXPECT_FLOAT_EQ(points[0].value, 1.0f);
}

TEST(AutomationEdits, DeleteRemovesOnlyTheSelection) {
	auto points = MakeDip();
	points[1].selected = false;
	DeleteSelected(points);

	ASSERT_EQ(points.size(), 1u);
	EXPECT_DOUBLE_EQ(points[0].beat, 2.02);
}
