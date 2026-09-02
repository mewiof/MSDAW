#include "PrecompHeader.h"
#include "TimelineClipOps.h"
#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include "Project.h"
#include "Undo/Actions.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace TimelineClipOps {

// hand a freshly cloned block its own copy of every note sequence it plays. a clone
// keeps pointing at the source's notes - that is exactly what makes Duplicate produce
// a linked ghost - but a clipboard and a paste have to stand on their own, or editing
// the source afterwards would rewrite what was copied. clips that shared a sequence
// with each OTHER keep sharing one, so a copied pair of ghosts pastes back as a pair
static void DetachSequences(const std::vector<std::shared_ptr<Clip>>& clips) {
	std::unordered_map<const MIDISequence*, std::shared_ptr<MIDISequence>> detached;
	for (const auto& clip : clips) {
		auto mIDIClip = std::dynamic_pointer_cast<MIDIClip>(clip);
		if (!mIDIClip)
			continue;
		auto& copy = detached[mIDIClip->GetSequence().get()];
		if (copy) {
			mIDIClip->AdoptSequence(copy);
		} else {
			mIDIClip->MakeUnique();
			copy = mIDIClip->GetSequence();
		}
	}
}

// scoped project lock. every mutation below goes through it: the audio thread walks
// the same clip vectors in Track::Process, and a multi-clip edit rewrites several of
// them in one go
static std::unique_lock<std::mutex> LockProject(Project* project) {
	if (project)
		return std::unique_lock<std::mutex>(project->GetMutex());
	return std::unique_lock<std::mutex>();
}

int FindTrackIndex(Project* project, const std::shared_ptr<Clip>& clip) {
	if (!project || !clip)
		return -1;
	auto& tracks = project->GetTracks();
	for (int i = 0; i < (int)tracks.size(); ++i) {
		for (const auto& c : tracks[i]->GetClips()) {
			if (c == clip)
				return i;
		}
	}
	return -1;
}

std::vector<ClipRef> ResolveSelection(EditorContext& context) {
	std::vector<ClipRef> refs;
	Project* project = context.GetProject();
	if (!project || context.state.selectedClips.empty())
		return refs;

	auto& tracks = project->GetTracks();
	for (int i = 0; i < (int)tracks.size(); ++i) {
		std::vector<ClipRef> onTrack;
		for (const auto& c : tracks[i]->GetClips()) {
			if (context.state.IsClipSelected(c))
				onTrack.push_back({c, tracks[i], i});
		}
		// a track holds its clips in insertion order, not time order
		std::sort(onTrack.begin(), onTrack.end(), [](const ClipRef& a, const ClipRef& b) {
			return a.clip->GetStartBeat() < b.clip->GetStartBeat();
		});
		refs.insert(refs.end(), onTrack.begin(), onTrack.end());
	}
	return refs;
}

void PruneSelection(EditorContext& context) {
	auto& state = context.state;
	if (state.selectedClips.empty() && !state.selectedClip)
		return;

	Project* project = context.GetProject();
	if (!project) {
		state.ClearClipSelection();
		return;
	}

	std::vector<const Clip*> live;
	for (const auto& t : project->GetTracks()) {
		for (const auto& c : t->GetClips())
			live.push_back(c.get());
	}
	std::sort(live.begin(), live.end());

	auto isLive = [&](const std::shared_ptr<Clip>& c) {
		return c && std::binary_search(live.begin(), live.end(), c.get());
	};

	state.selectedClips.erase(std::remove_if(state.selectedClips.begin(), state.selectedClips.end(),
											 [&](const std::shared_ptr<Clip>& c) { return !isLive(c); }),
							  state.selectedClips.end());

	if (!isLive(state.selectedClip))
		state.selectedClip = state.selectedClips.empty() ? nullptr : state.selectedClips.front();
}

// ================================================================
// SELECTION GESTURES
// ================================================================

double SnapMarqueeBeat(EditorContext& context, double beat) {
	if (beat < 0.0)
		beat = 0.0;
	// shift is the timeline's free-placement modifier. it doubles as "add to the
	// selection" on a marquee, exactly as it does in the automation lane, so a
	// shift-drag both extends the selection and places its edges off the grid
	if (ImGui::GetIO().KeyShift || context.state.timelineGrid <= 0.0)
		return beat;
	return std::round(beat / context.state.timelineGrid) * context.state.timelineGrid;
}

std::vector<std::shared_ptr<Clip>> ClipsInBox(Project* project, int trackFrom, int trackTo, double beatFrom, double beatTo) {
	std::vector<std::shared_ptr<Clip>> hits;
	if (!project)
		return hits;

	int minTrack = std::min(trackFrom, trackTo);
	int maxTrack = std::max(trackFrom, trackTo);
	double minBeat = std::min(beatFrom, beatTo);
	double maxBeat = std::max(beatFrom, beatTo);

	auto& tracks = project->GetTracks();
	minTrack = std::max(minTrack, 0);
	maxTrack = std::min(maxTrack, (int)tracks.size() - 1);

	for (int i = minTrack; i <= maxTrack; ++i) {
		std::vector<std::shared_ptr<Clip>> onTrack;
		for (const auto& c : tracks[i]->GetClips()) {
			// strict overlap, so a box that stops exactly on a clip's start edge does
			// not drag that clip in with it. a box dragged perfectly straight down has
			// no width to overlap with at all, and is treated as the line it is
			bool caught = (minBeat == maxBeat)
							  ? (c->GetStartBeat() <= minBeat && c->GetEndBeat() >= minBeat)
							  : (c->GetStartBeat() < maxBeat && c->GetEndBeat() > minBeat);
			if (caught)
				onTrack.push_back(c);
		}
		std::sort(onTrack.begin(), onTrack.end(), [](const std::shared_ptr<Clip>& a, const std::shared_ptr<Clip>& b) {
			return a->GetStartBeat() < b->GetStartBeat();
		});
		hits.insert(hits.end(), onTrack.begin(), onTrack.end());
	}
	return hits;
}

void SelectRangeTo(EditorContext& context, const std::shared_ptr<Clip>& target) {
	Project* project = context.GetProject();
	auto anchor = context.state.selectedClip;
	if (!project || !target || !anchor || anchor == target) {
		context.state.AddClipToSelection(target);
		return;
	}

	int anchorTrack = FindTrackIndex(project, anchor);
	int targetTrack = FindTrackIndex(project, target);
	if (anchorTrack < 0 || targetTrack < 0) {
		context.state.AddClipToSelection(target);
		return;
	}

	double beatFrom = std::min(anchor->GetStartBeat(), target->GetStartBeat());
	double beatTo = std::max(anchor->GetEndBeat(), target->GetEndBeat());
	auto hits = ClipsInBox(project, anchorTrack, targetTrack, beatFrom, beatTo);
	// the clicked clip keeps the focus, so a further shift-click extends from it
	context.state.SetClipSelection(std::move(hits), target);
}

void SelectAll(EditorContext& context) {
	Project* project = context.GetProject();
	if (!project)
		return;
	std::vector<std::shared_ptr<Clip>> all;
	for (const auto& t : project->GetTracks()) {
		for (const auto& c : t->GetClips())
			all.push_back(c);
	}
	context.state.SetClipSelection(std::move(all), context.state.selectedClip);
}

// ================================================================
// EDITS
// ================================================================

void DuplicateSelection(EditorContext& context) {
	Project* project = context.GetProject();
	auto refs = ResolveSelection(context);
	if (!project || refs.empty())
		return;

	// the block moves over by its own length, so a bar of drums lands in the next bar
	// instead of every clip stacking onto its own tail
	double minStart = refs.front().clip->GetStartBeat();
	double maxEnd = refs.front().clip->GetEndBeat();
	for (const auto& r : refs) {
		minStart = std::min(minStart, r.clip->GetStartBeat());
		maxEnd = std::max(maxEnd, r.clip->GetEndBeat());
	}
	double span = maxEnd - minStart;
	if (span < kMinClipDurationBeats)
		return;

	ClipEditScope scope(project, context.undoManager, "Duplicate clip");
	for (const auto& r : refs)
		scope.Touch(r.track);

	std::vector<std::shared_ptr<Clip>> clones;
	{
		auto lock = LockProject(project);
		for (const auto& r : refs) {
			auto clone = CloneClip(r.clip);
			if (!clone)
				continue;
			clone->SetStartBeat(r.clip->GetStartBeat() + span);
			r.track->AddClip(clone);
			clones.push_back(clone);
		}
	}
	if (clones.empty())
		return;

	context.state.SetClipSelection(std::move(clones));
	scope.Commit();
}

void DeleteSelection(EditorContext& context) {
	Project* project = context.GetProject();
	auto refs = ResolveSelection(context);
	if (!project || refs.empty())
		return;

	ClipEditScope scope(project, context.undoManager, "Delete clip");
	for (const auto& r : refs)
		scope.Touch(r.track);

	{
		auto lock = LockProject(project);
		for (const auto& r : refs)
			r.track->RemoveClip(r.clip);
	}

	context.state.ClearClipSelection();
	scope.Commit();
}

void ToggleSelectionEnabled(EditorContext& context) {
	Project* project = context.GetProject();
	auto refs = ResolveSelection(context);
	if (!project || refs.empty())
		return;

	// a mixed selection resolves to one state rather than scattering further: the
	// focused clip decides which way the whole block flips
	auto focus = context.state.selectedClip;
	bool reference = focus ? focus->IsEnabled() : refs.front().clip->IsEnabled();
	bool enabled = !reference;

	ClipEditScope scope(project, context.undoManager, enabled ? "Activate clip" : "Deactivate clip");
	for (const auto& r : refs)
		scope.Touch(r.track);

	{
		auto lock = LockProject(project);
		for (const auto& r : refs)
			r.clip->SetEnabled(enabled);
	}
	scope.Commit();
}

bool SelectionHasAudio(EditorContext& context) {
	for (const auto& clip : context.state.selectedClips)
		if (std::dynamic_pointer_cast<AudioClip>(clip))
			return true;
	return false;
}

void ReverseSelection(EditorContext& context) {
	Project* project = context.GetProject();
	auto refs = ResolveSelection(context);
	if (!project || refs.empty())
		return;

	// the offset each clip started from, kept alongside it: the mirror the reverse
	// applies is derived from the clip's reach, and the undo step pins the offset back
	// rather than trusting that reach to still be the same when it runs
	std::vector<std::pair<std::shared_ptr<AudioClip>, double>> reversed;
	{
		auto lock = LockProject(project);
		const double projectBpm = project->GetTransport().GetBpm();
		for (const auto& r : refs) {
			auto ac = std::dynamic_pointer_cast<AudioClip>(r.clip);
			if (!ac)
				continue; // a note list has no back to play from
			reversed.emplace_back(ac, ac->GetOffset());
			ac->Reverse(projectBpm);
		}
	}

	if (reversed.empty())
		return;

	// not a ClipEditScope: what changed is the sample buffer, which a clip snapshot does
	// not carry. one transaction so a block of clips still undoes in a single step
	context.undoManager.BeginTransaction("Reverse clip");
	for (const auto& [clip, beforeOffset] : reversed)
		context.undoManager.Push(std::make_unique<ReverseClipAction>(project, clip, beforeOffset, clip->GetOffset()));
	context.undoManager.EndTransaction();
}

void SplitSelectionAt(EditorContext& context, double beat) {
	Project* project = context.GetProject();
	auto refs = ResolveSelection(context);
	if (!project || refs.empty())
		return;

	ClipEditScope scope(project, context.undoManager, "Split clip");
	for (const auto& r : refs)
		scope.Touch(r.track);

	std::vector<std::shared_ptr<Clip>> selection;
	bool anySplit = false;
	{
		auto lock = LockProject(project);
		for (const auto& r : refs) {
			selection.push_back(r.clip);

			double start = r.clip->GetStartBeat();
			double end = r.clip->GetEndBeat();
			// a cut landing on (or a hair inside) an edge would leave a sliver too
			// small to see or grab, so those clips are left whole
			if (beat <= start + kMinClipDurationBeats || beat >= end - kMinClipDurationBeats)
				continue;

			auto rightSide = CloneClip(r.clip);
			if (!rightSide)
				continue;

			double consumed = beat - start;
			r.clip->SetDuration(consumed);
			rightSide->SetStartBeat(beat);
			rightSide->SetDuration(end - beat);
			rightSide->SetOffset(r.clip->GetOffset() + consumed);
			r.track->AddClip(rightSide);
			selection.push_back(rightSide);
			anySplit = true;
		}
	}
	if (!anySplit)
		return;

	context.state.SetClipSelection(std::move(selection), context.state.selectedClip);
	scope.Commit();
}

void CopySelection(EditorContext& context, TimelineInteractionState& interaction) {
	auto refs = ResolveSelection(context);
	if (refs.empty())
		return;

	double minStart = refs.front().clip->GetStartBeat();
	int minTrack = refs.front().trackIndex;
	for (const auto& r : refs) {
		minStart = std::min(minStart, r.clip->GetStartBeat());
		minTrack = std::min(minTrack, r.trackIndex);
	}

	interaction.clipboard.clear();
	std::vector<std::shared_ptr<Clip>> clones;
	for (const auto& r : refs) {
		auto clone = CloneClip(r.clip);
		if (!clone)
			continue;
		clones.push_back(clone);
		interaction.clipboard.push_back({clone, r.clip->GetStartBeat() - minStart, r.trackIndex - minTrack});
	}
	DetachSequences(clones);
}

void PasteAt(EditorContext& context, TimelineInteractionState& interaction, double anchorBeat, int anchorTrack) {
	Project* project = context.GetProject();
	if (!project || interaction.clipboard.empty())
		return;
	if (anchorBeat < 0.0)
		anchorBeat = 0.0;

	auto& tracks = project->GetTracks();
	ClipEditScope scope(project, context.undoManager, "Paste clip");

	// resolve every landing lane first: a paste that cannot place a clip drops that
	// entry rather than shifting it somewhere the user did not point at
	struct Landing {
		std::shared_ptr<Track> track;
		std::shared_ptr<Clip> clip;
		double start;
	};
	std::vector<Landing> landings;
	for (const auto& entry : interaction.clipboard) {
		int targetIdx = anchorTrack + entry.trackOffset;
		if (targetIdx < 0 || targetIdx >= (int)tracks.size())
			continue;
		if (!tracks[targetIdx]->AcceptsClips())
			continue;
		auto clone = CloneClip(entry.clip);
		if (!clone)
			continue;
		landings.push_back({tracks[targetIdx], clone, anchorBeat + entry.beatOffset});
		scope.Touch(tracks[targetIdx]);
	}
	if (landings.empty())
		return;

	std::vector<std::shared_ptr<Clip>> pasted;
	for (auto& l : landings)
		pasted.push_back(l.clip);
	// each paste is its own block: without this it would stay linked to the clipboard,
	// which means to every other paste of the same clipboard as well
	DetachSequences(pasted);

	{
		auto lock = LockProject(project);
		for (auto& l : landings) {
			l.clip->SetStartBeat(l.start);
			l.track->AddClip(l.clip);
		}
	}

	context.state.SetClipSelection(std::move(pasted));
	scope.Commit();
}

// ================================================================
// DRAG GEOMETRY
// ================================================================

DraggedClipGeometry ComputeDragGeometry(EditorContext& context, const TimelineInteractionState& interaction, const DragClipEntry& entry) {
	DraggedClipGeometry geom;
	geom.trackIdx = entry.trackIdx;
	geom.start = entry.startBeat;
	geom.duration = entry.duration;
	geom.offset = entry.offset;

	Project* project = context.GetProject();
	double minDuration = std::max(context.state.timelineGrid, kMinClipDurationBeats);

	// the gesture measured its deltas on the clip it started from; every other member
	// of the selection travels by the same amount, so the block keeps its shape
	if (interaction.dragState == DragState::Moving) {
		geom.start = std::max(0.0, entry.startBeat + (interaction.dragCurrentBeat - interaction.dragOriginalStart));
		geom.trackIdx = entry.trackIdx + (interaction.dragTargetTrackIdx - interaction.dragSourceTrackIdx);
	} else if (interaction.dragState == DragState::ResizingRight) {
		double newDur = entry.duration + (interaction.dragCurrentDuration - interaction.dragOriginalDuration);
		newDur = std::max(newDur, minDuration);

		// an audio clip cannot be stretched past the end of the file it plays
		if (auto audioClip = std::dynamic_pointer_cast<AudioClip>(entry.clip)) {
			double projectBpm = project ? project->GetTransport().GetBpm() : 120.0;
			double maxAllowed = audioClip->GetMaxDurationInBeats(projectBpm) - entry.offset;
			if (maxAllowed < minDuration)
				maxAllowed = minDuration;
			newDur = std::min(newDur, maxAllowed);
		}
		geom.duration = newDur;
	} else if (interaction.dragState == DragState::ResizingLeft) {
		double delta = interaction.dragCurrentBeat - interaction.dragOriginalStart;
		double end = entry.startBeat + entry.duration;
		double newStart = entry.startBeat + delta;
		double newOffset = entry.offset + delta;

		// dragging past the head of the source material stops at its first sample
		if (newOffset < 0.0) {
			newStart = entry.startBeat - entry.offset;
			newOffset = 0.0;
		}
		if (newStart < 0.0) {
			newOffset += -newStart;
			newStart = 0.0;
		}
		if (newStart > end - minDuration) {
			double correction = newStart - (end - minDuration);
			newStart -= correction;
			newOffset = std::max(0.0, newOffset - correction);
		}

		geom.start = newStart;
		geom.duration = end - newStart;
		geom.offset = newOffset;
	}
	return geom;
}

void CommitDrag(EditorContext& context, TimelineInteractionState& interaction, PendingClipMove& pendingMove) {
	Project* project = context.GetProject();
	if (!project || interaction.dragEntries.empty())
		return;

	auto& tracks = project->GetTracks();

	if (interaction.dragState == DragState::Moving) {
		bool beatChanged = interaction.dragCurrentBeat != interaction.dragOriginalStart;
		bool trackChanged = interaction.dragTargetTrackIdx != interaction.dragSourceTrackIdx;
		if (!beatChanged && !trackChanged)
			return;

		// a move erases from one track's clip vector and pushes onto another's, both
		// of which the caller is iterating - hand it back to be applied afterwards
		for (const auto& entry : interaction.dragEntries) {
			DraggedClipGeometry geom = ComputeDragGeometry(context, interaction, entry);
			if (geom.trackIdx < 0 || geom.trackIdx >= (int)tracks.size()) {
				// an out-of-range landing cancels the whole move: the block travels as
				// one, so placing part of it somewhere else would tear it apart
				pendingMove.entries.clear();
				return;
			}
			pendingMove.entries.push_back({entry.clip, entry.trackIdx, geom.trackIdx, geom.start});
		}
		pendingMove.valid = !pendingMove.entries.empty();
		return;
	}

	// a resize never changes which track a clip is on, so it can be applied in place
	const char* name = "Resize clip";
	ClipEditScope scope(project, context.undoManager, name);
	for (const auto& entry : interaction.dragEntries) {
		if (entry.trackIdx >= 0 && entry.trackIdx < (int)tracks.size())
			scope.Touch(tracks[entry.trackIdx]);
	}

	bool changed = false;
	{
		auto lock = LockProject(project);
		for (const auto& entry : interaction.dragEntries) {
			if (entry.trackIdx < 0 || entry.trackIdx >= (int)tracks.size())
				continue;
			DraggedClipGeometry geom = ComputeDragGeometry(context, interaction, entry);
			if (geom.start == entry.startBeat && geom.duration == entry.duration && geom.offset == entry.offset)
				continue;
			entry.clip->SetStartBeat(geom.start);
			entry.clip->SetDuration(geom.duration);
			entry.clip->SetOffset(geom.offset);
			tracks[entry.trackIdx]->ResolveOverlaps(entry.clip);
			changed = true;
		}
	}
	if (changed)
		scope.Commit();
}

} // namespace TimelineClipOps
