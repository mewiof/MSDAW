#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <memory>

#include "Clips/AudioClip.h"
#include "Processors/AutoSidechainProcessor.h"
#include "Project.h"
#include "Track.h"

// ================================================================
// RENDER TRACK TO A NEW TRACK
// ================================================================

// bouncing a track runs the same offline pass an export does, but rooted at one track
// instead of the whole graph. what these pin down is where the result lands (a sibling
// straight after the source, after the whole subtree for a group), that the source is
// left alone, and that the render hears the track even when the current mix does not

namespace {

	// a scratch directory of its own per test, so a bounce writing "<name> Render.wav"
	// never collides with a file another test left behind
	class RenderDirectory {
	public:
		explicit RenderDirectory(const std::string& name)
			: mPath(std::filesystem::temp_directory_path() / ("msdaw-render-" + name)) {
			std::filesystem::remove_all(mPath);
		}
		~RenderDirectory() { std::filesystem::remove_all(mPath); }

		std::string String() const { return mPath.string(); }
		const std::filesystem::path& Path() const { return mPath; }
	private:
		std::filesystem::path mPath;
	};

	// four beats of full-scale tone at 120 bpm, loud enough that any attenuation on the
	// way through the graph shows up as a peak well under one
	std::shared_ptr<AudioClip> ToneClip(double startBeat = 0.0) {
		auto clip = std::make_shared<AudioClip>();
		clip->GenerateTestSignal(48000.0, 2.0); // two seconds is four beats at 120
		clip->SetWarpingEnabled(false);
		clip->SetStartBeat(startBeat);
		clip->SetDuration(4.0);
		return clip;
	}

	float PeakOf(const std::shared_ptr<AudioClip>& clip) {
		float peak = 0.0f;
		for (float s : clip->GetSamples())
			peak = std::max(peak, std::abs(s));
		return peak;
	}

	// how loud the bounce is on average. peak is no use for measuring a compressor: its
	// attack lets the first transient through at full level, so the loudest sample is the
	// same whether the detector was fed or not
	double RmsOf(const std::shared_ptr<AudioClip>& clip) {
		const auto& samples = clip->GetSamples();
		if (samples.empty())
			return 0.0;
		double sum = 0.0;
		for (float s : samples)
			sum += (double)s * s;
		return std::sqrt(sum / (double)samples.size());
	}

	// the audio clip a bounce left on `track`, or null if there is none
	std::shared_ptr<AudioClip> RenderedClip(const std::shared_ptr<Track>& track) {
		if (!track || track->GetClips().empty())
			return nullptr;
		return std::dynamic_pointer_cast<AudioClip>(track->GetClips().front());
	}

} // namespace

TEST(RenderTrack, TheBounceLandsOnANewTrackRightAfterTheSource) {
	RenderDirectory directory("after-source");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.CreateTrack();
	project.GetTracks()[0]->SetName("Drums");
	project.GetTracks()[0]->AddClip(ToneClip());

	auto rendered = project.RenderTrackToNewTrack(0, directory.String());

	ASSERT_TRUE(rendered != nullptr);
	ASSERT_EQ(project.GetTracks().size(), 3u);
	EXPECT_EQ(project.GetTracks()[1], rendered);
	EXPECT_EQ(rendered->GetName(), "Drums Render");
	EXPECT_EQ(rendered->GetParent(), nullptr);
}

TEST(RenderTrack, TheBouncedClipHoldsTheTracksAudio) {
	RenderDirectory directory("audio");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.GetTracks()[0]->AddClip(ToneClip());

	auto rendered = project.RenderTrackToNewTrack(0, directory.String());
	ASSERT_TRUE(rendered != nullptr);

	auto clip = RenderedClip(rendered);
	ASSERT_TRUE(clip != nullptr);
	EXPECT_GT(clip->GetTotalFileFrames(), 0u);
	EXPECT_GT(PeakOf(clip), 0.1f); // the tone made it through, not silence
	EXPECT_DOUBLE_EQ(clip->GetStartBeat(), 0.0);
	EXPECT_FALSE(clip->IsWarpingEnabled()); // a bounce is already at the project tempo
}

// the source is what the user is bouncing FROM: adding a rendered copy must not disturb
// it, or the two would double up the next time the arrangement plays
TEST(RenderTrack, TheSourceTrackIsLeftAlone) {
	RenderDirectory directory("source-intact");
	Project project;
	project.Initialize();
	project.CreateTrack();
	auto source = project.GetTracks()[0];
	source->SetMute(true);
	source->AddClip(ToneClip());

	ASSERT_TRUE(project.RenderTrackToNewTrack(0, directory.String()) != nullptr);

	EXPECT_EQ(source->GetClips().size(), 1u);
	EXPECT_TRUE(source->GetMute()); // lifted for the render, put back after it
}

// a muted track that renders to silence would be a useless bounce: asking for the audio
// is asking past whatever the mix is currently doing with it
TEST(RenderTrack, AMutedTrackStillRendersItsAudio) {
	RenderDirectory directory("muted");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.GetTracks()[0]->SetMute(true);
	project.GetTracks()[0]->AddClip(ToneClip());

	auto rendered = project.RenderTrackToNewTrack(0, directory.String());
	ASSERT_TRUE(rendered != nullptr);
	EXPECT_GT(PeakOf(RenderedClip(rendered)), 0.1f);
}

// solo is an audition state, not part of the arrangement. a track bounced while some
// other track is soloed still has to come out with its own audio in it
TEST(RenderTrack, ASoloElsewhereDoesNotSilenceTheBounce) {
	RenderDirectory directory("solo");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.CreateTrack();
	project.GetTracks()[0]->AddClip(ToneClip());
	project.GetTracks()[1]->SetSolo(true);

	auto rendered = project.RenderTrackToNewTrack(0, directory.String());
	ASSERT_TRUE(rendered != nullptr);
	EXPECT_GT(PeakOf(RenderedClip(rendered)), 0.1f);
}

// the track's own fader is part of what it contributes, so the bounce has to carry it -
// dropping it at unity on a fresh track is what reproduces the sound
TEST(RenderTrack, TheBounceIsPostFader) {
	RenderDirectory quiet("fader-quiet");
	RenderDirectory loud("fader-loud");

	auto peakAtVolume = [&](float db, const RenderDirectory& directory) {
		Project project;
		project.Initialize();
		project.CreateTrack();
		project.GetTracks()[0]->AddClip(ToneClip());
		project.GetTracks()[0]->GetVolumeParameter()->value = db;
		auto rendered = project.RenderTrackToNewTrack(0, directory.String());
		return rendered ? PeakOf(RenderedClip(rendered)) : 0.0f;
	};

	const float atUnity = peakAtVolume(0.0f, loud);
	const float pulledDown = peakAtVolume(-20.0f, quiet);

	EXPECT_GT(atUnity, 0.1f);
	EXPECT_LT(pulledDown, atUnity * 0.5f);
}

// a group's sound is everything under it summed through its own chain, and the new
// track has to end up outside the group rather than as its last child
TEST(RenderTrack, AGroupRendersItsChildrenAndLandsOutsideItself) {
	RenderDirectory directory("group");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.CreateTrack();
	project.GetTracks()[0]->AddClip(ToneClip());
	project.GetTracks()[1]->AddClip(ToneClip());
	project.GroupSelectedTracks({0, 1});

	// GroupSelectedTracks inserts the header ahead of its members
	ASSERT_EQ(project.GetTracks().size(), 3u);
	auto group = project.GetTracks()[0];
	ASSERT_TRUE(group->IsGroup());

	auto rendered = project.RenderTrackToNewTrack(0, directory.String());
	ASSERT_TRUE(rendered != nullptr);
	ASSERT_EQ(project.GetTracks().size(), 4u);

	// after the whole group, not inside it
	EXPECT_EQ(project.GetTracks()[3], rendered);
	EXPECT_EQ(rendered->GetParent(), nullptr);
	EXPECT_GT(PeakOf(RenderedClip(rendered)), 0.1f);
}

// a track inside a group bounces to a sibling, so the copy stays in the same group and
// keeps feeding the same chain
TEST(RenderTrack, ATrackInsideAGroupBouncesIntoThatGroup) {
	RenderDirectory directory("in-group");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.CreateTrack();
	project.GetTracks()[0]->AddClip(ToneClip());
	project.GetTracks()[1]->AddClip(ToneClip());
	project.GroupSelectedTracks({0, 1});
	auto group = project.GetTracks()[0];

	auto rendered = project.RenderTrackToNewTrack(1, directory.String());
	ASSERT_TRUE(rendered != nullptr);
	EXPECT_EQ(rendered->GetParent(), group);
	EXPECT_EQ(project.GetTracks()[2], rendered);
}

// an explicit range is the time selection the user drew, and the clip has to land on it
// rather than at the start of the timeline
TEST(RenderTrack, ARangeBouncesOnlyThatStretchAndLandsOnIt) {
	RenderDirectory directory("range");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.GetTracks()[0]->AddClip(ToneClip());

	auto rendered = project.RenderTrackToNewTrack(0, directory.String(), 1.0, 3.0);
	ASSERT_TRUE(rendered != nullptr);

	auto clip = RenderedClip(rendered);
	ASSERT_TRUE(clip != nullptr);
	EXPECT_DOUBLE_EQ(clip->GetStartBeat(), 1.0);
	EXPECT_NEAR(clip->GetDuration(), 2.0, 1e-6);
	// two beats at 120 bpm is one second of audio
	EXPECT_NEAR((double)clip->GetTotalFileFrames() / clip->GetSampleRate(), 1.0, 1e-3);
}

// with no range given the bounce covers the track's own content plus a tail, so a
// reverb still ringing past the last clip is not cut off
TEST(RenderTrack, TheDefaultRangeCoversTheTrackPlusATail) {
	RenderDirectory directory("tail");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.GetTracks()[0]->AddClip(ToneClip(4.0)); // beats 4..8

	auto rendered = project.RenderTrackToNewTrack(0, directory.String());
	ASSERT_TRUE(rendered != nullptr);

	auto clip = RenderedClip(rendered);
	ASSERT_TRUE(clip != nullptr);
	EXPECT_DOUBLE_EQ(clip->GetStartBeat(), 0.0);
	EXPECT_NEAR(clip->GetDuration(), 12.0, 1e-6); // to beat 8, plus four beats of tail
}

// nothing to bounce means nothing added: a track that quietly appeared holding silence
// is worse than the menu item appearing to do nothing
TEST(RenderTrack, AnEmptyTrackAddsNothing) {
	RenderDirectory directory("empty");
	Project project;
	project.Initialize();
	project.CreateTrack();

	EXPECT_EQ(project.RenderTrackToNewTrack(0, directory.String()), nullptr);
	EXPECT_EQ(project.GetTracks().size(), 1u);
}

TEST(RenderTrack, AnOutOfRangeIndexAddsNothing) {
	RenderDirectory directory("bad-index");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.GetTracks()[0]->AddClip(ToneClip());

	EXPECT_EQ(project.RenderTrackToNewTrack(7, directory.String()), nullptr);
	EXPECT_EQ(project.RenderTrackToNewTrack(-1, directory.String()), nullptr);
	EXPECT_EQ(project.GetTracks().size(), 1u);
}

// bouncing the same track twice must not overwrite the first take: a clip in the
// project is still pointing at that file
TEST(RenderTrack, ASecondBounceWritesItsOwnFile) {
	RenderDirectory directory("twice");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.GetTracks()[0]->SetName("Bass");
	project.GetTracks()[0]->AddClip(ToneClip());

	auto first = project.RenderTrackToNewTrack(0, directory.String());
	auto second = project.RenderTrackToNewTrack(0, directory.String());
	ASSERT_TRUE(first != nullptr);
	ASSERT_TRUE(second != nullptr);

	EXPECT_TRUE(std::filesystem::exists(directory.Path() / "Bass Render.wav"));
	EXPECT_TRUE(std::filesystem::exists(directory.Path() / "Bass Render 2.wav"));
}

// track names are free text and end up in a filename; a name a path cannot carry has
// to still produce a file rather than a silently failed bounce
TEST(RenderTrack, ATrackNamedWithPathCharactersStillBounces) {
	RenderDirectory directory("odd-name");
	Project project;
	project.Initialize();
	project.CreateTrack();
	project.GetTracks()[0]->SetName("Lead / Bass: *take 2*");
	project.GetTracks()[0]->AddClip(ToneClip());

	auto rendered = project.RenderTrackToNewTrack(0, directory.String());
	ASSERT_TRUE(rendered != nullptr);
	EXPECT_GT(RenderedClip(rendered)->GetTotalFileFrames(), 0u);
}

// a bounce walks one subtree, so the track ducking this one is not in the walk at all.
// left unhandled the detector decays to silence and the clip comes out flat - audio that
// is silently wrong, which is worse than a bounce that visibly fails
TEST(RenderTrack, ABounceHearsTheSidechainDuckingIt) {
	RenderDirectory ducked("sidechain-ducked");
	RenderDirectory plain("sidechain-plain");

	// track 0 is the source, track 1 carries the tone the ducking is measured on
	auto levelWithSource = [&](bool wireUpSidechain, const RenderDirectory& directory) {
		Project project;
		project.Initialize();
		project.PrepareToPlay(48000.0);
		project.CreateTrack();
		project.CreateTrack();
		project.GetTracks()[0]->AddClip(ToneClip());
		project.GetTracks()[1]->AddClip(ToneClip());

		auto sidechain = std::make_shared<AutoSidechainProcessor>();
		// Follow, not the default Duck: Duck fires a fixed envelope off transients, and
		// a sustained tone only ever trips it once. Follow compresses continuously, so
		// whether the detector is being fed shows up across the whole bounce
		for (const auto& p : sidechain->GetParameters()) {
			if (p->name == "Mode")
				p->value = 0.0f;
		}
		if (wireUpSidechain)
			sidechain->SetSourceTrackId(project.GetTracks()[0]->GetId());
		project.GetTracks()[1]->AddProcessor(sidechain);
		sidechain->PrepareToPlay(48000.0);

		auto rendered = project.RenderTrackToNewTrack(1, directory.String());
		return rendered ? RmsOf(RenderedClip(rendered)) : 0.0;
	};

	const double unducked = levelWithSource(false, plain);
	const double withSource = levelWithSource(true, ducked);

	EXPECT_GT(unducked, 0.05);
	// the source plays all the way through the bounce, so a detector that is actually
	// being fed holds the whole thing down hard - well past anything a stray transient
	// or a single block of leakage could account for
	EXPECT_LT(withSource, unducked * 0.5);
}

// the render borrows the transport to run offline, and has to hand it back untouched
TEST(RenderTrack, TheTransportSurvivesABounce) {
	RenderDirectory directory("transport");
	Project project;
	project.Initialize();
	project.PrepareToPlay(48000.0);
	project.CreateTrack();
	project.GetTracks()[0]->AddClip(ToneClip());

	project.GetTransport().SetPosition(12345);
	project.GetTransport().SetLoopEnabled(true);

	ASSERT_TRUE(project.RenderTrackToNewTrack(0, directory.String()) != nullptr);

	EXPECT_EQ(project.GetTransport().GetPosition(), 12345);
	EXPECT_TRUE(project.GetTransport().IsLoopEnabled());
	EXPECT_DOUBLE_EQ(project.GetTransport().GetSampleRate(), 48000.0);
	EXPECT_FALSE(project.GetTransport().IsPlaying());
}
