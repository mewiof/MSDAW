#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

#include "Clips/AudioClip.h"
#include "Project.h"
#include "Track.h"

// ================================================================
// AUDIO CLIP REVERSE
// ================================================================

// reversing is materialized into the sample buffer rather than flipped at read time, so
// what these assert is that the buffer really turns around AND that the clip's window
// travels with it. a window left where it was would come back playing a different part
// of the file, which is the failure that makes the feature useless on a trimmed clip

namespace {

	// a clip over the whole of a four second file, unwarped: eight beats at 120 bpm.
	// every frame carries its own index so a reversal is visible in a single sample
	std::shared_ptr<AudioClip> RampClip() {
		auto clip = std::make_shared<AudioClip>();
		clip->GenerateTestSignal(48000.0, 4.0);
		clip->SetWarpingEnabled(false);
		clip->SetStartBeat(0.0);
		clip->SetDuration(8.0);
		return clip;
	}

	// the sample at a frame, first channel
	float FrameAt(const std::shared_ptr<AudioClip>& clip, size_t frame) {
		return clip->GetSamples()[frame * (size_t)clip->GetNumChannels()];
	}

	// a 16-bit stereo wav whose two channels never hold the same value, so a flip that
	// reversed the raw interleaved vector instead of whole frames would show up as the
	// channels having traded places. written to disk because loading a file is the only
	// way into a clip's sample buffer from outside
	std::shared_ptr<AudioClip> StereoClipFromFile(const std::filesystem::path& path, size_t frames) {
		std::ofstream out(path, std::ios::binary);
		auto put32 = [&](uint32_t v) { out.write((const char*)&v, 4); };
		auto put16 = [&](uint16_t v) { out.write((const char*)&v, 2); };
		const uint32_t dataBytes = (uint32_t)(frames * 4);
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
		for (size_t i = 0; i < frames; ++i) {
			put16((uint16_t)(int16_t)(i + 1));	// left counts up
			put16((uint16_t)(int16_t)-(int)(i + 1)); // right is its negative
		}
		out.close();

		auto clip = std::make_shared<AudioClip>();
		clip->LoadFromFile(path.string());
		return clip;
	}

} // namespace

TEST(AudioClipReverse, ReversingFlipsTheSampleBuffer) {
	auto clip = RampClip();
	const size_t frames = (size_t)clip->GetTotalFileFrames();
	const float firstBefore = FrameAt(clip, 0);
	const float lastBefore = FrameAt(clip, frames - 1);

	clip->Reverse(120.0);

	EXPECT_TRUE(clip->IsReversed());
	EXPECT_FLOAT_EQ(FrameAt(clip, 0), lastBefore);
	EXPECT_FLOAT_EQ(FrameAt(clip, frames - 1), firstBefore);
}

// interleaved data reverses frame-wise, not sample-wise: flipping the raw vector would
// swap left and right on every frame and quietly invert the stereo image
TEST(AudioClipReverse, ReversingKeepsTheChannelsInOrder) {
	auto path = std::filesystem::temp_directory_path() / "msdaw-reverse-stereo.wav";
	std::filesystem::remove(path);

	const size_t frames = 64;
	auto clip = StereoClipFromFile(path, frames);
	ASSERT_EQ(clip->GetNumChannels(), 2);
	ASSERT_EQ(clip->GetTotalFileFrames(), frames);
	const std::vector<float> before = clip->GetSamples();

	clip->Reverse(120.0);

	for (size_t i = 0; i < frames; ++i) {
		EXPECT_FLOAT_EQ(clip->GetSamples()[i * 2 + 0], before[(frames - 1 - i) * 2 + 0]);
		EXPECT_FLOAT_EQ(clip->GetSamples()[i * 2 + 1], before[(frames - 1 - i) * 2 + 1]);
		// the left channel counted up and the right down, so a vector-wise flip would
		// land a negative sample on the left here
		EXPECT_GT(clip->GetSamples()[i * 2 + 0], 0.0f);
		EXPECT_LT(clip->GetSamples()[i * 2 + 1], 0.0f);
	}

	std::filesystem::remove(path);
}

// the point of the feature on a trimmed clip: the window has to move to the other end
// of the file so the clip plays the slice it was cut around, backwards
TEST(AudioClipReverse, ReversingMirrorsTheClipsWindow) {
	auto clip = RampClip();
	clip->SetOffset(2.0);   // one second in, of four
	clip->SetDuration(4.0); // the middle two seconds

	clip->Reverse(120.0);

	// eight beats of file, a window of four starting at two: the mirror is 8 - 2 - 4
	EXPECT_DOUBLE_EQ(clip->GetOffset(), 2.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 4.0);
}

// an off-centre window is where a mirror that does nothing would show up
TEST(AudioClipReverse, AnOffCentreWindowMovesToTheOtherEnd) {
	auto clip = RampClip();
	clip->SetOffset(0.0);
	clip->SetDuration(2.0); // the first second of the file

	clip->Reverse(120.0);

	// the first second, reversed, now lives in the last two beats of the flipped file
	EXPECT_DOUBLE_EQ(clip->GetOffset(), 6.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 2.0);
}

// the clip has to come out playing the same audio: the first frame the window reaches
// after the reverse is the last frame it reached before it
TEST(AudioClipReverse, TheWindowStillCoversTheSameAudio) {
	auto clip = RampClip();
	clip->SetOffset(2.0);
	clip->SetDuration(2.0); // file seconds 1..2

	const double framesPerBeat = 48000.0 * 0.5; // 120 bpm
	const size_t windowStart = (size_t)(clip->GetOffset() * framesPerBeat);
	const size_t windowEnd = (size_t)((clip->GetOffset() + clip->GetDuration()) * framesPerBeat) - 1;
	const float firstBefore = FrameAt(clip, windowStart);
	const float lastBefore = FrameAt(clip, windowEnd);

	clip->Reverse(120.0);

	const size_t newStart = (size_t)(clip->GetOffset() * framesPerBeat);
	const size_t newEnd = (size_t)((clip->GetOffset() + clip->GetDuration()) * framesPerBeat) - 1;
	EXPECT_FLOAT_EQ(FrameAt(clip, newStart), lastBefore);
	EXPECT_FLOAT_EQ(FrameAt(clip, newEnd), firstBefore);
}

// undo runs the same operation, so it has to land exactly back on the original
TEST(AudioClipReverse, ReversingTwiceIsIdentity) {
	auto clip = RampClip();
	clip->SetOffset(3.0);
	clip->SetDuration(2.5);
	const std::vector<float> before = clip->GetSamples();

	clip->Reverse(120.0);
	clip->Reverse(120.0);

	EXPECT_FALSE(clip->IsReversed());
	EXPECT_DOUBLE_EQ(clip->GetOffset(), 3.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 2.5);
	EXPECT_EQ(clip->GetSamples(), before);
}

// the mirror is read in the clip's own beat units, so a transposed clip - whose file
// covers a different number of beats - still lands on the same audio
TEST(AudioClipReverse, TheMirrorFollowsTheClipsReach) {
	auto clip = RampClip();
	clip->SetTransposeSemitones(12.0); // twice the speed, four beats of reach
	clip->SetOffset(1.0);
	clip->SetDuration(1.0);

	clip->Reverse(120.0);

	// four beats of reach, a window of one starting at one: 4 - 1 - 1
	EXPECT_DOUBLE_EQ(clip->GetOffset(), 2.0);
}

// a granular warp mode reads the file at the grid's pace rather than the file's, so its
// reach is a different number of beats again and the mirror has to use that one
TEST(AudioClipReverse, TheMirrorFollowsAWarpedClipsReach) {
	auto clip = RampClip();
	clip->SetWarpingEnabled(true);
	clip->SetSegmentBpm(60.0); // four seconds at 60 bpm is four beats of file
	clip->SetOffset(1.0);
	clip->SetDuration(2.0);

	clip->Reverse(120.0);

	EXPECT_DOUBLE_EQ(clip->GetOffset(), 1.0); // 4 - 1 - 2
}

// a clip with no file behind it has nothing to flip and no reach to mirror against;
// leaving the window alone is what keeps it recoverable if the file comes back
TEST(AudioClipReverse, AClipWithNoFileBehindItKeepsItsWindow) {
	auto clip = std::make_shared<AudioClip>();
	clip->SetOffset(2.0);
	clip->SetDuration(4.0);

	clip->Reverse(120.0);

	EXPECT_TRUE(clip->IsReversed());
	EXPECT_DOUBLE_EQ(clip->GetOffset(), 2.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 4.0);
}

// the wav on disk is always the forward one, so a load has to flip the buffer back
// itself - and must NOT mirror the window again, which is already the mirrored one
TEST(AudioClipReverse, AReversedClipSurvivesASaveAndLoad) {
	auto directory = std::filesystem::temp_directory_path() / "msdaw-reverse-round-trip";
	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory);
	auto wavPath = directory / "tone.wav";
	auto projectPath = directory / "reverse.msdaw";

	// a real file on disk, because the load path re-reads the samples from it
	{
		Project source;
		source.Initialize();
		source.CreateTrack();
		auto clip = RampClip();
		source.GetTracks()[0]->AddClip(clip);
		ASSERT_TRUE(source.RenderAudio(wavPath.string(), 0.0, 8.0, 48000.0));
	}

	float expectedFirst = 0.0f;
	{
		Project saved;
		saved.Initialize();
		saved.CreateTrack();
		auto clip = std::make_shared<AudioClip>();
		ASSERT_TRUE(clip->LoadFromFile(wavPath.string()));
		clip->SetDuration(4.0);
		clip->SetOffset(1.0);
		saved.GetTracks()[0]->AddClip(clip);

		clip->Reverse(saved.GetTransport().GetBpm());
		expectedFirst = FrameAt(clip, (size_t)(clip->GetOffset() * 48000.0 * 0.5));
		saved.Save(projectPath.string());
	}

	Project loaded;
	loaded.Initialize();
	loaded.Load(projectPath.string());

	ASSERT_EQ(loaded.GetTracks().size(), 1u);
	ASSERT_EQ(loaded.GetTracks()[0]->GetClips().size(), 1u);
	auto reloaded = std::dynamic_pointer_cast<AudioClip>(loaded.GetTracks()[0]->GetClips()[0]);
	ASSERT_TRUE(reloaded != nullptr);

	EXPECT_TRUE(reloaded->IsReversed());
	EXPECT_DOUBLE_EQ(reloaded->GetOffset(), 3.0); // 8 - 1 - 4, saved and not re-mirrored
	EXPECT_FLOAT_EQ(FrameAt(reloaded, (size_t)(reloaded->GetOffset() * 48000.0 * 0.5)), expectedFirst);

	std::filesystem::remove_all(directory);
}

// a clip copied off a reversed one carries its own flipped buffer, so reversing the
// copy back must not disturb the original - this is what duplicate and paste rely on
TEST(AudioClipReverse, ACopyReversesIndependently) {
	auto clip = RampClip();
	clip->Reverse(120.0);

	auto copy = std::make_shared<AudioClip>(*clip);
	copy->Reverse(120.0);

	EXPECT_TRUE(clip->IsReversed());
	EXPECT_FALSE(copy->IsReversed());
	EXPECT_FLOAT_EQ(FrameAt(clip, 0), FrameAt(copy, copy->GetTotalFileFrames() - 1));
}
