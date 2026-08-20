#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include "Transport.h"

// ================================================================
// TRANSPORT
// ================================================================

TEST(Transport, DefaultsToStoppedAt120Bpm) {
	Transport transport;

	EXPECT_FALSE(transport.IsPlaying());
	EXPECT_EQ(transport.GetPosition(), 0);
	EXPECT_DOUBLE_EQ(transport.GetBpm(), 120.0);
	EXPECT_DOUBLE_EQ(transport.GetSampleRate(), 48000.0);
}

// stop and pause differ only in what they do to the playhead, and every
// caller that picks the wrong one loses the user's position silently
TEST(Transport, StopRewindsButPauseKeepsPosition) {
	Transport transport;
	transport.SetPosition(4800);

	transport.Pause();
	EXPECT_FALSE(transport.IsPlaying());
	EXPECT_EQ(transport.GetPosition(), 4800);

	transport.Play();
	transport.Stop();
	EXPECT_FALSE(transport.IsPlaying());
	EXPECT_EQ(transport.GetPosition(), 0);
}

TEST(Transport, AdvanceOnlyMovesWhilePlaying) {
	Transport transport;

	transport.Advance(512);
	EXPECT_EQ(transport.GetPosition(), 0);

	transport.Play();
	transport.Advance(512);
	transport.Advance(512);
	EXPECT_EQ(transport.GetPosition(), 1024);
}

// the time model: beats author, samples play. a wrong conversion here is the
// single most expensive bug in the project, so pin the arithmetic
TEST(Transport, SamplesPerBeatFollowsBpmAndSampleRate) {
	Transport transport;

	const auto samplesPerBeat = [&]() {
		return transport.GetSampleRate() * 60.0 / transport.GetBpm();
	};

	EXPECT_DOUBLE_EQ(samplesPerBeat(), 24000.0);

	transport.SetBpm(60.0);
	EXPECT_DOUBLE_EQ(samplesPerBeat(), 48000.0);

	transport.SetSampleRate(44100.0);
	transport.SetBpm(140.0);
	EXPECT_DOUBLE_EQ(samplesPerBeat(), 44100.0 * 60.0 / 140.0);
}

TEST(Transport, LoopRangeRoundTrips) {
	Transport transport;

	EXPECT_FALSE(transport.IsLoopEnabled());

	transport.SetLoopRange(1000, 5000);
	transport.SetLoopEnabled(true);

	EXPECT_TRUE(transport.IsLoopEnabled());
	EXPECT_EQ(transport.GetLoopStart(), 1000);
	EXPECT_EQ(transport.GetLoopEnd(), 5000);
}
