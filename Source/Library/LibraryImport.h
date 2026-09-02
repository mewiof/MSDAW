#pragma once
#include <memory>
#include <string>

class Clip;
class Project;
class Track; // FD

// turning a file on disk into something in the arrangement. every way a file gets
// in - the library explorer's drag and its context menu, a drop from the OS onto
// the timeline - comes through here, so "what is importable" is decided in exactly
// one place and a headless test can drive the same code the drop does
//
// none of these push undo actions: a clip landing on a track is a ClipEditScope to
// its caller and a new track is a TrackTopologyAction, and only the caller knows
// whether the two are one step or two
namespace LibraryImport {

	// a clip built from the file, sized against the project tempo and named after it.
	// null when the extension is not one the DAW imports, or the file fails to parse
	std::shared_ptr<Clip> MakeClip(const std::string& path, double bpm);

	// put that clip on a track at a beat. takes the project lock, since the audio
	// thread walks the clip list of every track it renders
	bool ImportToTrack(Project* project, const std::shared_ptr<Track>& track,
					   const std::string& path, double startBeat);

	// ... on a track of its own, appended after the last one and named after the
	// file. returns the new track's index, or -1 when nothing was imported
	int ImportToNewTrack(Project* project, const std::string& path, double startBeat);

} // namespace LibraryImport
