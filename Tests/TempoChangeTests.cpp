#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

#include "Clips/AudioClip.h"
#include "Project.h"
#include "Track.h"

// ================================================================
// TEMPO CHANGES
// ================================================================

// an unwarped audio clip plays at the file's own speed, so its length on the grid is
// a tempo reading of a fixed stretch of seconds and has to be re-read when the project
// tempo moves. the failure that matters is a clip whose beat length lands on zero: the
// timeline hands that to ImGui as a zero-size item, which asserts outright

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

	// a project holding one track with `clip` on it, at the default 120 bpm
	void HostClip(Project& project, std::shared_ptr<Clip> clip) {
		project.Initialize();
		project.CreateTrack();
		project.GetTracks()[0]->AddClip(clip);
	}

} // namespace

// the tempo has to be re-readable: a drop that truncates the clip and never gives the
// audio back when the tempo returns loses the arrangement a bar at a time
TEST(TempoChange, AnUnwarpedClipSurvivesATempoRoundTrip) {
	Project project;
	auto clip = FourSecondClip();
	HostClip(project, clip);

	project.SetBpm(60.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 4.0); // same four seconds, half the beats

	project.SetBpm(120.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 8.0);
}

// the crash: an offset deep into the file plus a tempo drop used to clamp the duration
// straight to zero
TEST(TempoChange, ATempoDropNeverCollapsesAClipToZeroBeats) {
	Project project;
	auto clip = FourSecondClip();
	clip->SetStartBeat(4.0);
	clip->SetOffset(6.0); // 3 s into a 4 s file at 120 bpm
	clip->SetDuration(2.0);
	HostClip(project, clip);

	project.SetBpm(20.0);

	EXPECT_GT(clip->GetDuration(), 0.0);
	EXPECT_DOUBLE_EQ(clip->GetOffset(), 1.0); // still 3 s into the file
}

// a warped clip is stretched onto the grid, so the same audio always covers the same
// beats and the tempo must not touch it
TEST(TempoChange, AWarpedClipKeepsItsBeatLength) {
	Project project;
	auto clip = FourSecondClip();
	clip->SetWarpingEnabled(true);
	clip->SetSegmentBpm(120.0);
	clip->SetOffset(2.0);
	clip->SetDuration(6.0); // the rest of the file from two beats in
	HostClip(project, clip);

	project.SetBpm(60.0);

	EXPECT_DOUBLE_EQ(clip->GetDuration(), 6.0);
	EXPECT_DOUBLE_EQ(clip->GetOffset(), 2.0);
}

// a project whose audio files have moved loads clips with no samples behind them. those
// report zero beats of content, which used to clamp them to nothing on the first tempo
// edit. an unverifiable length is not a wrong one: re-read it like any other so the window
// still points at the same audio if the file comes back
TEST(TempoChange, AClipWithNoFileBehindItIsRetimedNotWiped) {
	Project project;
	auto clip = std::make_shared<AudioClip>();
	clip->SetStartBeat(4.0);
	clip->SetDuration(4.0);
	HostClip(project, clip);

	project.SetBpm(140.0);
	EXPECT_GT(clip->GetDuration(), 0.0);

	project.SetBpm(120.0);
	EXPECT_DOUBLE_EQ(clip->GetDuration(), 4.0);
}

// the same zero-length trap without the transport: transposing up shortens an unwarped
// clip's reach, and an offset past the new end used to leave nothing at all
TEST(TempoChange, TransposingPastTheFileEndLeavesAGrabbableClip) {
	auto clip = FourSecondClip();
	clip->SetOffset(6.0);
	clip->SetDuration(2.0);
	clip->SetTransposeSemitones(12.0); // half the playback time, so four beats of reach

	clip->ValidateDuration(120.0);

	EXPECT_GT(clip->GetDuration(), 0.0);
}

// the tempo re-read is driven from the audio thread off the master track's BPM parameter,
// so a load that left that parameter disagreeing with the transport would retime every
// clip in the project the user just opened
TEST(TempoChange, LoadingAProjectDoesNotRetimeItsClips) {
	auto path = std::filesystem::temp_directory_path() / "msdaw-tempo-round-trip.msdaw";
	std::filesystem::remove(path);

	{
		Project saved;
		auto clip = FourSecondClip();
		HostClip(saved, clip);
		saved.SetBpm(90.0);
		saved.Save(path.string());
	}

	Project loaded;
	loaded.Initialize();
	loaded.Load(path.string());

	auto clip = loaded.GetTracks()[0]->GetClips()[0];
	double durationOnDisk = clip->GetDuration();

	// one silent block is all it takes: the parameter is read at the top of every one
	std::vector<float> buffer(512 * 2, 0.0f);
	std::vector<MIDIMessage> noLiveEvents;
	loaded.ProcessBlock(buffer.data(), 512, 2, noLiveEvents);

	EXPECT_DOUBLE_EQ(clip->GetDuration(), durationOnDisk);

	std::filesystem::remove(path);
}
