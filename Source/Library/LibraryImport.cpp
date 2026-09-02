#include "PrecompHeader.h"
#include "LibraryImport.h"

#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include "Library/FileBrowser.h"
#include "PathText.h"
#include "Project.h"
#include "Track.h"

#include <filesystem>
#include <mutex>

namespace {

	// beats are the authoring unit, so an audio file's length in seconds only becomes
	// a clip length once the project tempo is known. a MIDI file carries its own
	// length in beats already and sets it while parsing
	double DurationInBeats(const AudioClip& clip, double bpm) {
		const double sampleRate = clip.GetSampleRate();
		if (sampleRate <= 0.0 || bpm <= 0.0)
			return 0.0;
		return ((double)clip.GetTotalFileFrames() / sampleRate) * (bpm / 60.0);
	}

} // namespace

std::shared_ptr<Clip> LibraryImport::MakeClip(const std::string& path, double bpm) {
	const std::filesystem::path file = PathText::ToPath(path);

	switch (FileBrowser::ClassifyExtension(PathText::FromPath(file.extension()))) {
		case FileKind::Audio: {
			auto clip = std::make_shared<AudioClip>();
			clip->SetName(PathText::FromPath(file.filename()));
			if (!clip->LoadFromFile(path))
				return nullptr;
			const double beats = DurationInBeats(*clip, bpm);
			if (beats > 0.0)
				clip->SetDuration(beats);
			return clip;
		}
		case FileKind::MIDI: {
			auto clip = std::make_shared<MIDIClip>();
			clip->SetName(PathText::FromPath(file.filename()));
			if (!clip->LoadFromFile(path))
				return nullptr;
			return clip;
		}
		default:
			// a project is opened, not imported, and everything else is not ours
			return nullptr;
	}
}

bool LibraryImport::ImportToTrack(Project* project, const std::shared_ptr<Track>& track,
								  const std::string& path, double startBeat) {
	if (!project || !track || !track->AcceptsClips())
		return false;

	auto clip = MakeClip(path, project->GetTransport().GetBpm());
	if (!clip)
		return false;

	clip->SetStartBeat(startBeat < 0.0 ? 0.0 : startBeat);

	std::lock_guard<std::mutex> lock(project->GetMutex());
	track->AddClip(clip);
	return true;
}

int LibraryImport::ImportToNewTrack(Project* project, const std::string& path, double startBeat) {
	if (!project)
		return -1;

	// the file is read before the track exists, so a file that turns out not to load
	// does not leave an empty track behind
	auto clip = MakeClip(path, project->GetTransport().GetBpm());
	if (!clip)
		return -1;

	clip->SetStartBeat(startBeat < 0.0 ? 0.0 : startBeat);

	project->CreateTrack();
	auto& tracks = project->GetTracks();
	if (tracks.empty())
		return -1;

	auto track = tracks.back();
	track->SetName(PathText::FromPath(PathText::ToPath(path).stem()));

	std::lock_guard<std::mutex> lock(project->GetMutex());
	track->AddClip(clip);
	return (int)tracks.size() - 1;
}
