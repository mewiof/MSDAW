#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <memory>

#include "Clips/AudioClip.h"

// ================================================================
// CLIP PITCH AND WARP RETIMING
// ================================================================

// transposing an unwarped clip is tape speed: the same audio comes out faster or slower,
// so the clip has to stretch and shrink on the grid to match. clamping alone only ever
// took length away - the clip lost its tail on the way up and never got it back on the
// way down, and the window slid onto different audio than the one it was cut to

namespace {

	// a clip covering the whole of a four second file, unwarped: eight beats at 120 bpm
	std::shared_ptr<AudioClip> FourSecondClip() {
		auto clip = std::make_shared<AudioClip>();
		clip->GenerateTestSignal(48000.0, 4.0);
		clip->SetWarpingEnabled(false);
		clip->SetStartBeat(0.0);
		clip->SetDuration(8.0);
		return clip;
	}

	// what a warp or pitch edit has to be wrapped in to leave the clip's geometry true
	void Transpose(const std::shared_ptr<AudioClip>& clip, double semitones, double projectBpm = 120.0) {
		double maxBefore = clip->GetMaxDurationInBeats(projectBpm);
		clip->SetTransposeSemitones(semitones);
		clip->RetimeForWarpChange(maxBefore, projectBpm);
	}

} // namespace

// an octave up halves the playback time, an octave down doubles it. the second half is
// the one that used to be missing entirely
TEST(AudioClipPitch, AnUnwarpedClipStretchesAndShrinksWithTheTranspose) {
	auto clip = FourSecondClip();

	Transpose(clip, 12.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 4.0);

	Transpose(clip, -12.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 16.0);

	Transpose(clip, 0.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 8.0);
}

// the clip is a window onto a slice of the file, and a cut is what fixed that slice.
// transposing may only change how long the slice takes to play, never which audio it is
TEST(AudioClipPitch, TransposingKeepsTheSliceTheClipWasCutTo) {
	auto clip = FourSecondClip();
	clip->SetOffset(2.0);   // one second into the file at 120 bpm
	clip->SetDuration(4.0); // the second of the file's four

	Transpose(clip, 12.0);
	EXPECT_DOUBLE_EQ(clip->GetOffset(), 1.0); // still one second in, at twice the speed
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 2.0);

	Transpose(clip, 0.0);
	EXPECT_DOUBLE_EQ(clip->GetOffset(), 2.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 4.0);
}

// the clamp is still the backstop: a window that already reached the end of the file
// cannot grow past it just because the transpose made room somewhere else
TEST(AudioClipPitch, AStretchedClipStillStopsAtTheFileEnd) {
	auto clip = FourSecondClip();

	Transpose(clip, -12.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 16.0); // the whole file, an octave down

	clip->SetDuration(32.0);
	clip->ValidateDuration(120.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 16.0);
}

// a granular warp mode holds the file to the grid, which is the whole point of warping:
// pitch moves without the clip changing length
TEST(AudioClipPitch, TransposingAWarpedClipLeavesItsLengthAlone) {
	auto clip = FourSecondClip();
	clip->SetWarpingEnabled(true);
	clip->SetWarpMode(WarpMode::Beats);
	clip->SetSegmentBpm(120.0);
	clip->SetOffset(2.0);
	clip->SetDuration(6.0);

	Transpose(clip, 7.0);

	EXPECT_DOUBLE_EQ(clip->GetOffset(), 2.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 6.0);
}

// Re-Pitch is deliberately varispeed, so it retimes like an unwarped clip even though
// warping is on
TEST(AudioClipPitch, ARePitchClipRetimesLikeAnUnwarpedOne) {
	auto clip = FourSecondClip();
	clip->SetWarpingEnabled(true);
	clip->SetWarpMode(WarpMode::RePitch);
	clip->SetSegmentBpm(120.0);

	Transpose(clip, 12.0);

	EXPECT_DOUBLE_EQ(clip->GetDuration(), 4.0);
}

// telling a warped clip its file was recorded at half the tempo means the file has to be
// played twice as fast to reach the grid, so it covers half the beats. this is what the
// /2 and x2 buttons beside the segment tempo do
TEST(AudioClipPitch, HalvingTheSegmentTempoHalvesAWarpedClip) {
	auto clip = FourSecondClip();
	clip->SetWarpingEnabled(true);
	clip->SetSegmentBpm(120.0);

	double maxBefore = clip->GetMaxDurationInBeats(120.0);
	clip->SetSegmentBpm(60.0);
	clip->RetimeForWarpChange(maxBefore, 120.0);

	EXPECT_DOUBLE_EQ(clip->GetDuration(), 4.0);
}

// a clip whose file has moved reports no reach at all. re-reading a window against that
// would wipe it; an unverifiable length is not a wrong one
TEST(AudioClipPitch, AClipWithNoFileBehindItIsNotRetimed) {
	auto clip = std::make_shared<AudioClip>();
	clip->SetDuration(4.0);
	clip->SetOffset(1.0);

	double maxBefore = clip->GetMaxDurationInBeats(120.0);
	clip->SetTransposeSemitones(12.0);
	clip->RetimeForWarpChange(maxBefore, 120.0);

	EXPECT_DOUBLE_EQ(clip->GetDuration(), 4.0);
	EXPECT_DOUBLE_EQ(clip->GetOffset(), 1.0);
}
