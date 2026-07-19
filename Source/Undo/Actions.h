#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include "Undo/UndoableAction.h"
#include "Undo/UndoManager.h"
#include "Parameter.h"
#include "Track.h"
#include "Project.h"
#include "Clip.h"
#include "Clips/MIDIClip.h"

// ---------------------------------------------------------------------------
// parameter value change (knob / slider / toggle / typed / reset-to-default).
// value is a plain float also read by the audio thread; writing it without a
// lock is consistent with how live edits already behave.
// ---------------------------------------------------------------------------
class ParameterChangeAction : public UndoableAction {
public:
	ParameterChangeAction(Parameter* param, float oldValue, float newValue)
		: mParam(param), mOld(oldValue), mNew(newValue) {}

	void Undo() override {
		if (mParam)
			mParam->value = mOld;
	}
	void Redo() override {
		if (mParam)
			mParam->value = mNew;
	}
	const char* Name() const override { return "Parameter change"; }
private:
	Parameter* mParam;
	float mOld;
	float mNew;
};

// ---------------------------------------------------------------------------
// device present/absent on a track. Holds the processor shared_ptr so the
// object (and every Parameter* inside it) stays alive across the whole history.
//   isInsert = true  -> the device was ADDED   (Redo inserts, Undo removes)
//   isInsert = false -> the device was REMOVED (Redo removes, Undo inserts)
// ---------------------------------------------------------------------------
class ProcessorPresenceAction : public UndoableAction {
public:
	ProcessorPresenceAction(Project* project, std::shared_ptr<Track> track,
							std::shared_ptr<AudioProcessor> proc, int index, bool isInsert)
		: mProject(project), mTrack(std::move(track)), mProc(std::move(proc)), mIndex(index), mIsInsert(isInsert) {}

	void Undo() override { mIsInsert ? DoRemove() : DoInsert(); }
	void Redo() override { mIsInsert ? DoInsert() : DoRemove(); }
	const char* Name() const override { return mIsInsert ? "Add device" : "Remove device"; }
private:
	void DoInsert() {
		if (!mTrack || !mProc)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		mTrack->InsertProcessor(mIndex, mProc);
	}
	void DoRemove() {
		if (!mTrack)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		auto& procs = mTrack->GetProcessors();
		// prefer the recorded index, but fall back to a search in case indices shifted
		if (mIndex >= 0 && mIndex < (int)procs.size() && procs[mIndex] == mProc) {
			mTrack->RemoveProcessor(mIndex);
			return;
		}
		for (int i = 0; i < (int)procs.size(); ++i) {
			if (procs[i] == mProc) {
				mTrack->RemoveProcessor(i);
				return;
			}
		}
	}

	Project* mProject;
	std::shared_ptr<Track> mTrack;
	std::shared_ptr<AudioProcessor> mProc;
	int mIndex;
	bool mIsInsert;
};

// reorder a device within a single track. from/to are plain final positions in
// the processor vector (not the "insert-before" convention), which makes the
// move trivially invertible
class ProcessorMoveAction : public UndoableAction {
public:
	ProcessorMoveAction(Project* project, std::shared_ptr<Track> track, int from, int to)
		: mProject(project), mTrack(std::move(track)), mFrom(from), mTo(to) {}

	void Undo() override { Apply(mTo, mFrom); }
	void Redo() override { Apply(mFrom, mTo); }
	const char* Name() const override { return "Move device"; }
private:
	void Apply(int from, int to) {
		if (!mTrack)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		auto& procs = mTrack->GetProcessors();
		if (from < 0 || from >= (int)procs.size() || to < 0 || to >= (int)procs.size())
			return;
		auto proc = procs[from];
		procs.erase(procs.begin() + from);
		procs.insert(procs.begin() + to, proc);
	}
	Project* mProject;
	std::shared_ptr<Track> mTrack;
	int mFrom;
	int mTo;
};

// ---------------------------------------------------------------------------
// track topology snapshot (order + parent links). One action covers create,
// remove, move, group and ungroup. All tracks are kept alive by the retained
// shared_ptrs in the snapshots
// ---------------------------------------------------------------------------
class TrackTopologyAction : public UndoableAction {
public:
	struct Entry {
		std::shared_ptr<Track> track;
		std::shared_ptr<Track> parent; // may be null
	};

	TrackTopologyAction(Project* project, std::vector<Entry> before, std::vector<Entry> after, const char* name)
		: mProject(project), mBefore(std::move(before)), mAfter(std::move(after)), mName(name) {}

	void Undo() override { ApplyState(mBefore); }
	void Redo() override { ApplyState(mAfter); }
	const char* Name() const override { return mName; }

	// snapshot the current track topology
	static std::vector<Entry> Snapshot(Project* project) {
		std::vector<Entry> entries;
		for (auto& t : project->GetTracks())
			entries.push_back({t, t->GetParent()});
		return entries;
	}

	// run a topology-mutating operation and record it as one undo step
	template <typename Fn>
	static void Record(UndoManager& undo, Project* project, const char* name, Fn&& fn) {
		if (!project) {
			fn();
			return;
		}
		auto before = Snapshot(project);
		fn();
		auto after = Snapshot(project);
		undo.Push(std::make_unique<TrackTopologyAction>(project, before, after, name));
	}
private:
	void ApplyState(const std::vector<Entry>& state) {
		std::vector<std::shared_ptr<Track>> order;
		order.reserve(state.size());
		for (const auto& e : state) {
			e.track->SetParent(e.parent);
			order.push_back(e.track);
		}
		mProject->RestoreTracks(std::move(order));
	}

	Project* mProject;
	std::vector<Entry> mBefore;
	std::vector<Entry> mAfter;
	const char* mName;
};

// ---------------------------------------------------------------------------
// clip snapshot for a single track (membership + geometry). Covers create,
// remove, move and resize including any overlap trimming/splitting they cause
// ---------------------------------------------------------------------------
class ClipSnapshotAction : public UndoableAction {
public:
	struct Entry {
		std::shared_ptr<Clip> clip;
		double start;
		double duration;
		double offset;
	};

	ClipSnapshotAction(Project* project, std::shared_ptr<Track> track,
					   std::vector<Entry> before, std::vector<Entry> after, const char* name)
		: mProject(project), mTrack(std::move(track)), mBefore(std::move(before)), mAfter(std::move(after)), mName(name) {}

	void Undo() override { ApplyState(mBefore); }
	void Redo() override { ApplyState(mAfter); }
	const char* Name() const override { return mName; }

	static std::vector<Entry> Snapshot(const std::shared_ptr<Track>& track) {
		std::vector<Entry> entries;
		for (auto& c : track->GetClips())
			entries.push_back({c, c->GetStartBeat(), c->GetDuration(), c->GetOffset()});
		return entries;
	}
private:
	void ApplyState(const std::vector<Entry>& state) {
		if (!mTrack)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		std::vector<std::shared_ptr<Clip>> clips;
		clips.reserve(state.size());
		for (const auto& e : state) {
			e.clip->SetStartBeat(e.start);
			e.clip->SetDuration(e.duration);
			e.clip->SetOffset(e.offset);
			clips.push_back(e.clip);
		}
		mTrack->SetClips(std::move(clips));
	}

	Project* mProject;
	std::shared_ptr<Track> mTrack;
	std::vector<Entry> mBefore;
	std::vector<Entry> mAfter;
	const char* mName;
};

// ---------------------------------------------------------------------------
// automation curve edit (add / drag / delete points). Replaces the curve's
// point list wholesale
// ---------------------------------------------------------------------------
class AutomationEditAction : public UndoableAction {
public:
	AutomationEditAction(Project* project, std::shared_ptr<Track> track, Parameter* param,
						 std::vector<AutomationPoint> before, std::vector<AutomationPoint> after)
		: mProject(project), mTrack(std::move(track)), mParam(param), mBefore(std::move(before)), mAfter(std::move(after)) {}

	void Undo() override { Apply(mBefore); }
	void Redo() override { Apply(mAfter); }
	const char* Name() const override { return "Automation edit"; }
private:
	void Apply(const std::vector<AutomationPoint>& points) {
		if (!mTrack || !mParam)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		mTrack->SetAutomationPoints(mParam, points);
	}
	Project* mProject;
	std::shared_ptr<Track> mTrack;
	Parameter* mParam;
	std::vector<AutomationPoint> mBefore;
	std::vector<AutomationPoint> mAfter;
};

// ---------------------------------------------------------------------------
// piano-roll note edit (add / delete / move / resize / nudge / velocity).
// Snapshots the whole note list before and after; the retained MIDIClip
// shared_ptr keeps the sequence alive across the history. Undo/Redo lock the
// project mutex because the audio thread iterates the same note vector
// ---------------------------------------------------------------------------
class NoteEditAction : public UndoableAction {
public:
	NoteEditAction(Project* project, std::shared_ptr<MIDIClip> clip,
				   std::vector<MIDINote> before, std::vector<MIDINote> after, const char* name)
		: mProject(project), mClip(std::move(clip)), mBefore(std::move(before)), mAfter(std::move(after)), mName(name) {}

	void Undo() override { Apply(mBefore); }
	void Redo() override { Apply(mAfter); }
	const char* Name() const override { return mName; }

	static std::vector<MIDINote> Snapshot(const std::shared_ptr<MIDIClip>& clip) {
		return clip ? clip->GetNotes() : std::vector<MIDINote>{};
	}
private:
	void Apply(const std::vector<MIDINote>& state) {
		if (!mClip)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		mClip->GetNotesEx() = state;
	}
	Project* mProject;
	std::shared_ptr<MIDIClip> mClip;
	std::vector<MIDINote> mBefore;
	std::vector<MIDINote> mAfter;
	const char* mName;
};
