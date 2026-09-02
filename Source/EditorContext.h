#pragma once
#include <algorithm>
#include <string>
#include <memory>
#include <set>
#include <vector>
#include "AudioEngine.h"
#include "Clip.h"
#include "PluginManager.h"
#include "AudioProcessor.h"
#include "Track.h"
#include "Undo/UndoManager.h"
#include "imgui.h"

struct EditorLayout {
	float transportHeight = 40.0f;	  // single control row - taller left a dead strip under the buttons
	// the device strip is a fixed height and nothing in it scrolls, so this number is
	// what every built-in device lays itself out inside. it is the shortest strip the
	// tallest of them still draws its own editor at full size in: the tab strip, the
	// rack's scrollbar and a device's own header eat ~83 px, and what is left is exactly
	// EQ Eight's globals column - eleven rows at full control height with the gaps
	// between them closed. take more away and those controls start shrinking; below
	// ~260 the device gives up on its editor and falls back to a parameter list
	float bottomPanelHeight = 300.0f;
	float libraryWidth = 200.0f;
	float libraryCollapsedWidth = 26.0f; // folded-away library: just the rail that brings it back
	float trackListWidth = 240.0f;
	float trackRowHeight = 80.0f;
	float trackCollapsedHeight = 22.0f; // height of a minimized (collapsed) track row
	float trackGap = 0.0f;				// TODO: remove this?

	void Scale(float scale) {
		transportHeight *= scale;
		bottomPanelHeight *= scale;
		libraryWidth *= scale;
		libraryCollapsedWidth *= scale;
		trackListWidth *= scale;
		trackRowHeight *= scale;
		trackCollapsedHeight *= scale;
		trackGap *= scale;
	}
};

enum class FollowMode {
	Page,
	Continuous
};

struct EditorState {
	float mainScale = 1.0f;
	float pixelsPerBeat = 60.0f;

	// main timeline snapping settings
	double timelineGrid = 0.25;
	int timelineGridNumerator = 1;
	int timelineGridDenominator = 4;

	float timelineScrollY = 0.0f; // master timeline scroll y
	float timelineScrollX = 0.0f; // master timeline scroll x
	bool restoreScroll = false;

	// timeline selection (beats)
	double selectionStart = 0.0;
	double selectionEnd = 0.0;

	// follow playback
	bool followPlayback = false;
	FollowMode followMode = FollowMode::Continuous; // continuous scroll mode

	// selection
	int selectedTrackIndex = 0;		   // primary selection
	std::set<int> multiSelectedTracks; // multi-selection

	// the one way to say "the user picked this track". the primary index and the
	// multi-selection set are drawn as one highlight and consumed as one by the
	// grouping commands, so a caller that sets only the index leaves the other
	// stale - the track list would keep lighting up the old row and Group would
	// act on it
	// a topology change (undo/redo, a removed track) can leave either half of the
	// selection pointing past the end of the list
	void ClampTrackSelection(int trackCount) {
		if (selectedTrackIndex >= trackCount)
			selectedTrackIndex = trackCount - 1;
		for (auto it = multiSelectedTracks.begin(); it != multiSelectedTracks.end();) {
			if (*it >= trackCount)
				it = multiSelectedTracks.erase(it);
			else
				++it;
		}
	}

	void SelectTrack(int index) {
		selectedTrackIndex = index;
		multiSelectedTracks.clear();
		if (index >= 0)
			multiSelectedTracks.insert(index);
	}

	// ---- clip selection ----
	// selectedClip is the FOCUSED member of selectedClips: the clip the piano roll and
	// the clip view edit, the one a drag anchors its deltas on, and the one drawn with
	// the accent outline. it is always also present in selectedClips, so a caller that
	// writes only the pointer leaves the other stale - every editing command reads the
	// vector. go through the helpers below instead
	std::shared_ptr<Clip> selectedClip = nullptr;
	std::vector<std::shared_ptr<Clip>> selectedClips;

	bool IsClipSelected(const std::shared_ptr<Clip>& clip) const {
		return clip && std::find(selectedClips.begin(), selectedClips.end(), clip) != selectedClips.end();
	}

	void ClearClipSelection() {
		selectedClips.clear();
		selectedClip = nullptr;
	}

	// exclusive select: the clip becomes the whole selection and the focus
	void SelectClip(std::shared_ptr<Clip> clip) {
		selectedClips.clear();
		if (clip)
			selectedClips.push_back(clip);
		selectedClip = std::move(clip);
	}

	// additive select (shift/ctrl-click, marquee): joins the selection and takes focus
	void AddClipToSelection(std::shared_ptr<Clip> clip) {
		if (!clip)
			return;
		if (!IsClipSelected(clip))
			selectedClips.push_back(clip);
		selectedClip = std::move(clip);
	}

	// ctrl-click on a clip already in the selection drops it back out. focus follows to
	// another member rather than going null, so the piano roll keeps showing something
	void ToggleClipSelection(const std::shared_ptr<Clip>& clip) {
		if (!clip)
			return;
		auto it = std::find(selectedClips.begin(), selectedClips.end(), clip);
		if (it == selectedClips.end()) {
			AddClipToSelection(clip);
			return;
		}
		selectedClips.erase(it);
		if (selectedClip == clip)
			selectedClip = selectedClips.empty() ? nullptr : selectedClips.back();
	}

	// replace the whole selection at once (marquee, paste, duplicate). focus falls back
	// to the first member when the caller has no particular clip in mind
	void SetClipSelection(std::vector<std::shared_ptr<Clip>> clips, std::shared_ptr<Clip> focus = nullptr) {
		selectedClips = std::move(clips);
		if (focus && IsClipSelected(focus))
			selectedClip = std::move(focus);
		else
			selectedClip = selectedClips.empty() ? nullptr : selectedClips.front();
	}

	// clipboard
	std::shared_ptr<AudioProcessor> processorClipboard = nullptr;

	// automation clipboard
	std::vector<AutomationPoint> automationClipboard;

	// window visibility
	bool showSettingsWindow = false;
	bool showHistoryWindow = false;

	// the arrangement's clip shortcuts sit on bare letters, and the computer MIDI
	// keyboard plays notes off those same letters. set by the arrangement each frame
	// and read by the keyboard on the next one: while it is true the keyboard leaves
	// the arrangement's letters alone, so deactivating a clip does not also play a note
	bool arrangementOwnsLetterKeys = false;

	// MIDI keyboard state
	bool isComputerMIDIKeyboardEnabled = true;
	int mIDIOctave = 3;		// base octave
	int mIDIVelocity = 100; // default velocity

	// active keyboard notes
	std::set<int> activeMIDINotes;

	// where the project lives on disk, empty until it has first been saved. the editor
	// writes it on every new/open/save; the views read it to put files they generate
	// (a bounced track's wav) beside the project rather than off in an app folder
	std::string projectPath;

	// os drag and drop
	std::string droppedPath;
	float dropX = 0.0f;
	float dropY = 0.0f;
	bool processDrop = false;

	// os drag preview
	bool isOsDragging = false;
	float osDragX = 0.0f;
	float osDragY = 0.0f;
	double lastOsDragTime = 0.0; // drag anti-flicker timer
};

struct EditorContext {
	AudioEngine& engine;
	EditorState state;
	EditorLayout layout;
	PluginManager pluginManager;
	UndoManager undoManager;

	// native window handle
	void* nativeWindowHandle = nullptr;

	EditorContext(AudioEngine& e)
		: engine(e) {}
	Project* GetProject() { return engine.GetProject(); }
};
