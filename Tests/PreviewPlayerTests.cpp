#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "PathText.h"
#include "PreviewPlayer.h"

// ================================================================
// LIBRARY PREVIEW
// ================================================================

// auditioning a file from the library, which is the one thing in the DAW that makes
// sound without the transport running. the player is driven here the way the audio
// callback drives it - hand it a file, then pull blocks out of it

namespace fs = std::filesystem;

// NOTE: the fixture stays out of an anonymous namespace - TEST_F names it from file
// scope, and a fixture hidden in one is not the class the macro would find
class PreviewPlayerTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
		mRoot = fs::temp_directory_path() / ("MSDAWPreview_" + std::string(info->name()));
		std::error_code ec;
		fs::remove_all(mRoot, ec);
		fs::create_directories(mRoot, ec);
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove_all(mRoot, ec);
	}

	// a 16-bit wav holding one constant value on every channel, so a block pulled out
	// of the player can be compared against a number rather than a waveform
	std::string WriteWav(const std::string& name, uint32_t frames, int channels, uint32_t sampleRate, float value) {
		const fs::path path = mRoot / name;
		std::ofstream out(path, std::ios::binary);
		auto put32 = [&](uint32_t v) { out.write((const char*)&v, 4); };
		auto put16 = [&](uint16_t v) { out.write((const char*)&v, 2); };
		const uint16_t blockAlign = (uint16_t)(channels * 2);
		const uint32_t dataBytes = frames * blockAlign;
		out.write("RIFF", 4);
		put32(36 + dataBytes);
		out.write("WAVE", 4);
		out.write("fmt ", 4);
		put32(16);
		put16(1);
		put16((uint16_t)channels);
		put32(sampleRate);
		put32(sampleRate * blockAlign);
		put16(blockAlign);
		put16(16);
		out.write("data", 4);
		put32(dataBytes);
		for (uint32_t i = 0; i < frames * (uint32_t)channels; ++i)
			put16((uint16_t)(int16_t)(value * 32768.0f));
		return path.string();
	}

	std::string PathOf(const std::string& name) const { return (mRoot / name).string(); }

	// one block out of the player, stereo interleaved, starting from silence
	std::vector<float> PullBlock(PreviewPlayer& player, unsigned int frames) {
		std::vector<float> block(frames * 2, 0.0f);
		player.ProcessBlock(block.data(), frames, 2);
		return block;
	}

	static bool IsSilent(const std::vector<float>& block) {
		for (float sample : block) {
			if (sample != 0.0f)
				return false;
		}
		return true;
	}

	fs::path mRoot;
};

TEST_F(PreviewPlayerTest, StartsSilentAndPlayingNothing) {
	PreviewPlayer player;
	EXPECT_FALSE(player.IsPlaying());
	EXPECT_TRUE(player.PlayingPath().empty());
	EXPECT_TRUE(IsSilent(PullBlock(player, 64)));
}

TEST_F(PreviewPlayerTest, PlaysAFileTheMomentItIsHandedOne) {
	PreviewPlayer player;
	player.SetOutputSampleRate(48000.0);
	const std::string path = WriteWav("kick.wav", 4800, 1, 48000, 0.5f);

	ASSERT_TRUE(player.Play(path));
	EXPECT_TRUE(player.IsPlaying());
	EXPECT_EQ(player.PlayingPath(), path);

	const std::vector<float> block = PullBlock(player, 64);
	for (size_t i = 0; i < block.size(); ++i)
		EXPECT_NEAR(block[i], 0.5f, 1e-4f) << "mono is auditioned on both sides, at sample " << i;
}

TEST_F(PreviewPlayerTest, RefusesAFileThatIsNotAudioAndKeepsPlayingWhatWas) {
	PreviewPlayer player;
	player.SetOutputSampleRate(48000.0);
	const std::string path = WriteWav("tone.wav", 4800, 1, 48000, 0.5f);
	ASSERT_TRUE(player.Play(path));
	PullBlock(player, 64);

	std::ofstream(mRoot / "readme.txt") << "not audio";
	EXPECT_FALSE(player.Play(PathOf("readme.txt")));
	EXPECT_FALSE(player.Play(PathOf("missing.wav")));

	EXPECT_TRUE(player.IsPlaying());
	EXPECT_EQ(player.PlayingPath(), path);
	EXPECT_FALSE(IsSilent(PullBlock(player, 64)));
}

// the audition is mixed in beside the project, not in place of it
TEST_F(PreviewPlayerTest, AddsIntoTheOutputRatherThanReplacingIt) {
	PreviewPlayer player;
	player.SetOutputSampleRate(48000.0);
	ASSERT_TRUE(player.Play(WriteWav("tone.wav", 4800, 1, 48000, 0.25f)));

	std::vector<float> block(128, 0.5f); // stand-in for the mix already in the buffer
	player.ProcessBlock(block.data(), 64, 2);

	for (float sample : block)
		EXPECT_NEAR(sample, 0.75f, 1e-4f);
}

TEST_F(PreviewPlayerTest, StopsPlayingWhenItRunsOffTheEndOfTheFile) {
	PreviewPlayer player;
	player.SetOutputSampleRate(48000.0);
	ASSERT_TRUE(player.Play(WriteWav("short.wav", 100, 1, 48000, 0.5f)));

	const std::vector<float> first = PullBlock(player, 64);
	EXPECT_FALSE(IsSilent(first));
	EXPECT_TRUE(player.IsPlaying());

	// the second block runs out 36 frames in and everything past that is silence
	const std::vector<float> second = PullBlock(player, 64);
	EXPECT_NEAR(second[0], 0.5f, 1e-4f);
	EXPECT_FLOAT_EQ(second[100], 0.0f);
	EXPECT_FALSE(player.IsPlaying());

	EXPECT_TRUE(IsSilent(PullBlock(player, 64)));

	// the panel stops calling it the playing row once the UI thread catches up
	player.Collect();
	EXPECT_TRUE(player.PlayingPath().empty());
}

TEST_F(PreviewPlayerTest, StopSilencesTheNextBlock) {
	PreviewPlayer player;
	player.SetOutputSampleRate(48000.0);
	ASSERT_TRUE(player.Play(WriteWav("tone.wav", 48000, 1, 48000, 0.5f)));
	ASSERT_FALSE(IsSilent(PullBlock(player, 64)));

	player.Stop();
	EXPECT_FALSE(player.IsPlaying());
	EXPECT_TRUE(player.PlayingPath().empty());
	EXPECT_TRUE(IsSilent(PullBlock(player, 64)));
}

// a sample recorded at another rate has to come out at the right pitch, which means
// stepping through it at the ratio of the two rates
TEST_F(PreviewPlayerTest, ResamplesAFileRecordedAtAnotherRate) {
	PreviewPlayer player;
	player.SetOutputSampleRate(48000.0);
	// 100 frames at 24 kHz is 200 frames of output at 48 kHz
	ASSERT_TRUE(player.Play(WriteWav("halfrate.wav", 100, 1, 24000, 0.5f)));

	const std::vector<float> block = PullBlock(player, 256);
	EXPECT_NEAR(block[0], 0.5f, 1e-4f);
	EXPECT_NEAR(block[199 * 2], 0.5f, 1e-4f) << "the file should still be playing at frame 199";
	EXPECT_FLOAT_EQ(block[200 * 2], 0.0f) << "and finished by frame 200";
	EXPECT_FALSE(player.IsPlaying());
}

TEST_F(PreviewPlayerTest, KeepsTheChannelsOfAStereoFileApart) {
	PreviewPlayer player;
	player.SetOutputSampleRate(48000.0);

	// left at +0.5, right at -0.5, written by hand so the two differ
	const fs::path path = mRoot / "stereo.wav";
	{
		std::ofstream out(path, std::ios::binary);
		auto put32 = [&](uint32_t v) { out.write((const char*)&v, 4); };
		auto put16 = [&](uint16_t v) { out.write((const char*)&v, 2); };
		const uint32_t frames = 256;
		const uint32_t dataBytes = frames * 4;
		out.write("RIFF", 4);
		put32(36 + dataBytes);
		out.write("WAVE", 4);
		out.write("fmt ", 4);
		put32(16);
		put16(1);
		put16(2);
		put32(48000);
		put32(48000 * 4);
		put16(4);
		put16(16);
		out.write("data", 4);
		put32(dataBytes);
		for (uint32_t i = 0; i < frames; ++i) {
			put16((uint16_t)(int16_t)16384);
			put16((uint16_t)(int16_t)-16384);
		}
	}

	ASSERT_TRUE(player.Play(path.string()));
	const std::vector<float> block = PullBlock(player, 64);
	for (unsigned int frame = 0; frame < 64; ++frame) {
		EXPECT_NEAR(block[frame * 2 + 0], 0.5f, 1e-4f);
		EXPECT_NEAR(block[frame * 2 + 1], -0.5f, 1e-4f);
	}
}

// starting one audition while another is running swaps the buffer over rather than
// layering the two
TEST_F(PreviewPlayerTest, ReplacesTheAuditionInFlight) {
	PreviewPlayer player;
	player.SetOutputSampleRate(48000.0);
	ASSERT_TRUE(player.Play(WriteWav("first.wav", 48000, 1, 48000, 0.5f)));
	PullBlock(player, 64);

	const std::string second = WriteWav("second.wav", 48000, 1, 48000, 0.25f);
	ASSERT_TRUE(player.Play(second));
	EXPECT_EQ(player.PlayingPath(), second);

	const std::vector<float> block = PullBlock(player, 64);
	for (float sample : block)
		EXPECT_NEAR(sample, 0.25f, 1e-4f) << "the first file should be gone, not mixed under the second";

	player.Collect(); // the first buffer is freed here, on this thread
	EXPECT_TRUE(player.IsPlaying());
}

// the audition opens the same UTF-8 path the browser lists, so a sample named in a
// script the code page has no room for is still playable
TEST_F(PreviewPlayerTest, AuditionsAFileTheAnsiCodePageCannotSpell) {
	PreviewPlayer player;
	player.SetOutputSampleRate(48000.0);

	const std::string ascii = WriteWav("scratch.wav", 4800, 1, 48000, 0.5f);
	const fs::path target = mRoot / L"キック.wav"; // "キック.wav"
	std::error_code ec;
	fs::rename(PathText::ToPath(ascii), target, ec);
	ASSERT_FALSE(ec);

	const std::string path = PathText::FromPath(target);
	ASSERT_TRUE(player.Play(path));
	EXPECT_EQ(player.PlayingPath(), path);

	const std::vector<float> block = PullBlock(player, 64);
	for (float sample : block)
		EXPECT_NEAR(sample, 0.5f, 1e-4f);
}
