#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include "Undo/UndoableAction.h"
#include "Undo/UndoManager.h"
#include "Parameter.h"
#include "ProcessorHost.h"
#include "Processors/ModulatorProcessor.h"
#include "Processors/RackProcessor.h"
#include "Track.h"
#include "Project.h"
#include "Clip.h"
#include "Clips/MIDIClip.h"
#include "Clips/AudioClip.h"

// ---------------------------------------------------------------------------
// parameter value change (knob / slider / toggle / typed / reset-to-default)
// value is a plain float also read by the audio thread; writing it without a
// lock is consistent with how live edits already behave
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
// a device chain, snapshotted whole. one action covers insert, remove, reorder,
// group, ungroup and a multi-device delete, on a track or on any rack chain
// nested inside one - "the devices in this host changed" is the only fact a
// device edit ever reports. the retained shared_ptrs keep every device (and
// every Parameter* inside it) alive across the whole history
// ---------------------------------------------------------------------------
class ProcessorChainAction : public UndoableAction {
public:
	using Chain = std::vector<std::shared_ptr<AudioProcessor>>;

	ProcessorChainAction(Project* project, std::shared_ptr<ProcessorHost> host,
						 Chain before, Chain after, const char* name)
		: mProject(project), mHost(std::move(host)), mBefore(std::move(before)), mAfter(std::move(after)), mName(name) {}

	void Undo() override { Apply(mBefore); }
	void Redo() override { Apply(mAfter); }
	const char* Name() const override { return mName; }

	static Chain Snapshot(const std::shared_ptr<ProcessorHost>& host) {
		return host ? host->GetProcessors() : Chain{};
	}
private:
	void Apply(const Chain& chain) {
		if (!mHost)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		mHost->SetProcessors(chain);
	}

	Project* mProject;
	std::shared_ptr<ProcessorHost> mHost;
	Chain mBefore;
	Chain mAfter;
	const char* mName;
};

// ---------------------------------------------------------------------------
// one device edit spanning several chains = one history entry. Touch() every
// host the edit is about to mutate (before mutating it), then Commit(). the
// same shape as ClipEditScope: hosts that came out unchanged are dropped, so
// dragging a device inside one rack still reads as a plain "Move device"
// ---------------------------------------------------------------------------
class DeviceEditScope {
public:
	DeviceEditScope(Project* project, UndoManager& undoManager, const char* name)
		: mProject(project), mUndo(undoManager), mName(name) {}

	// snapshotting a host twice would capture it mid-edit, so repeats are ignored
	void Touch(const std::shared_ptr<ProcessorHost>& host) {
		if (!host || !mProject)
			return;
		for (const auto& entry : mHosts) {
			if (entry.host == host)
				return;
		}
		mHosts.push_back({host, ProcessorChainAction::Snapshot(host)});
	}

	void Commit() {
		if (!mProject || mHosts.empty())
			return;
		mUndo.BeginTransaction(mName);
		for (auto& entry : mHosts) {
			auto after = ProcessorChainAction::Snapshot(entry.host);
			if (after != entry.before)
				mUndo.Push(std::make_unique<ProcessorChainAction>(mProject, entry.host, entry.before, std::move(after), mName));
		}
		mUndo.EndTransaction();
		mHosts.clear();
	}
private:
	struct HostEntry {
		std::shared_ptr<ProcessorHost> host;
		ProcessorChainAction::Chain before;
	};

	Project* mProject;
	UndoManager& mUndo;
	const char* mName;
	std::vector<HostEntry> mHosts;
};

// ---------------------------------------------------------------------------
// activating / deactivating devices. the flag is read by the audio thread on
// its way past each device, so the write takes the project lock; one action
// covers however many devices the selection held
// ---------------------------------------------------------------------------
class DeviceBypassAction : public UndoableAction {
public:
	struct Entry {
		std::shared_ptr<AudioProcessor> device;
		bool bypassed;
	};

	DeviceBypassAction(Project* project, std::vector<Entry> before, std::vector<Entry> after, const char* name)
		: mProject(project), mBefore(std::move(before)), mAfter(std::move(after)), mName(name) {}

	void Undo() override { Apply(mBefore); }
	void Redo() override { Apply(mAfter); }
	const char* Name() const override { return mName; }
private:
	void Apply(const std::vector<Entry>& state) {
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		for (const auto& entry : state) {
			if (entry.device)
				entry.device->SetBypassed(entry.bypassed);
		}
	}
	Project* mProject;
	std::vector<Entry> mBefore;
	std::vector<Entry> mAfter;
	const char* mName;
};

// ---------------------------------------------------------------------------
// the curated parameter list a device panel shows. one action covers a whole
// configuring session - every control touched in the plugin's editor between
// switching Add on and off - as well as a single parameter dropped and the list
// cleared. no lock: the audio thread never reads this list, it only decides what
// the panel draws
// ---------------------------------------------------------------------------
class DevicePanelAction : public UndoableAction {
public:
	DevicePanelAction(std::shared_ptr<AudioProcessor> device, std::vector<int> before, std::vector<int> after, const char* name)
		: mDevice(std::move(device)), mBefore(std::move(before)), mAfter(std::move(after)), mName(name) {}

	void Undo() override { Apply(mBefore); }
	void Redo() override { Apply(mAfter); }
	const char* Name() const override { return mName; }
private:
	void Apply(const std::vector<int>& state) {
		if (mDevice)
			mDevice->SetPanelParameters(state);
	}
	std::shared_ptr<AudioProcessor> mDevice;
	std::vector<int> mBefore;
	std::vector<int> mAfter;
	const char* mName;
};

// ---------------------------------------------------------------------------
// everything a rack-level edit changes: its name, its color, its macro titles,
// colors and mappings, and the chain list itself. one snapshot covers the lot,
// and the chains are held by shared_ptr so undoing a deleted chain brings the
// devices in it back too
// ---------------------------------------------------------------------------
class RackStateAction : public UndoableAction {
public:
	RackStateAction(Project* project, std::shared_ptr<RackProcessor> rack,
					RackProcessor::RackState before, RackProcessor::RackState after, const char* name)
		: mProject(project), mRack(std::move(rack)), mBefore(std::move(before)), mAfter(std::move(after)), mName(name) {}

	void Undo() override { Apply(mBefore); }
	void Redo() override { Apply(mAfter); }
	const char* Name() const override { return mName; }
private:
	void Apply(const RackProcessor::RackState& state) {
		if (!mRack)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		mRack->ApplyState(state);
	}
	Project* mProject;
	std::shared_ptr<RackProcessor> mRack;
	RackProcessor::RackState mBefore;
	RackProcessor::RackState mAfter;
	const char* mName;
};

// ---------------------------------------------------------------------------
// a modulator's patterns and its target list, snapshotted whole. one action
// covers a step drag, a randomize, a target added or dropped and a range edit -
// none of which are parameter values, so none of them are already covered by
// ParameterChangeAction. the retained shared_ptr keeps the device alive across
// the whole history
// ---------------------------------------------------------------------------
class ModulatorStateAction : public UndoableAction {
public:
	ModulatorStateAction(Project* project, std::shared_ptr<ModulatorProcessor> modulator,
						 ModulatorProcessor::State before, ModulatorProcessor::State after, std::string name)
		: mProject(project), mModulator(std::move(modulator)), mBefore(std::move(before)),
		  mAfter(std::move(after)), mName(std::move(name)) {}

	void Undo() override { Apply(mBefore); }
	void Redo() override { Apply(mAfter); }
	const char* Name() const override { return mName.c_str(); }
private:
	void Apply(const ModulatorProcessor::State& state) {
		if (!mModulator || !mProject)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		mModulator->ApplyState(state);
	}
	Project* mProject;
	std::shared_ptr<ModulatorProcessor> mModulator;
	ModulatorProcessor::State mBefore;
	ModulatorProcessor::State mAfter;
	// the name is built at the call site (which pattern changed), so it cannot be
	// the borrowed literal every other action here carries
	std::string mName;
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
		bool enabled;

		// memberwise compare so a multi-track edit can tell which tracks it actually
		// changed and leave the rest out of the history entry
		bool operator==(const Entry&) const = default;
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
			entries.push_back({c, c->GetStartBeat(), c->GetDuration(), c->GetOffset(), c->IsEnabled()});
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
			e.clip->SetEnabled(e.enabled);
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
// flip a clip between active and deactivated. the sequencer reads the flag on
// the audio thread, so the write takes the project lock; the undo step is a
// plain clip snapshot, which carries activation along with the geometry
// ---------------------------------------------------------------------------
inline void ToggleClipEnabled(Project* project, UndoManager& undoManager,
							  const std::shared_ptr<Track>& track,
							  const std::shared_ptr<Clip>& clip) {
	if (!project || !track || !clip)
		return;

	std::vector<ClipSnapshotAction::Entry> before = ClipSnapshotAction::Snapshot(track);
	{
		std::lock_guard<std::mutex> lock(project->GetMutex());
		clip->SetEnabled(!clip->IsEnabled());
	}
	undoManager.Push(std::make_unique<ClipSnapshotAction>(project, track, before,
														  ClipSnapshotAction::Snapshot(track),
														  clip->IsEnabled() ? "Activate clip" : "Deactivate clip"));
}

// ---------------------------------------------------------------------------
// one clip edit spanning several tracks = one history entry. Touch() every track
// the edit is about to mutate (before mutating it), then Commit(): each touched
// track contributes a ClipSnapshotAction and the set is collapsed into a single
// transaction. tracks that came out unchanged are dropped, so a multi-clip drag
// that only ever touched one lane still reads as a plain "Move clip"
// ---------------------------------------------------------------------------
class ClipEditScope {
public:
	ClipEditScope(Project* project, UndoManager& undoManager, const char* name)
		: mProject(project), mUndo(undoManager), mName(name) {}

	// snapshotting a track twice would capture it mid-edit, so repeats are ignored
	void Touch(const std::shared_ptr<Track>& track) {
		if (!track || !mProject)
			return;
		for (const auto& e : mTracks) {
			if (e.track == track)
				return;
		}
		mTracks.push_back({track, ClipSnapshotAction::Snapshot(track)});
	}

	bool Empty() const { return mTracks.empty(); }

	void Commit() {
		if (!mProject || mTracks.empty())
			return;
		mUndo.BeginTransaction(mName);
		for (auto& e : mTracks) {
			auto after = ClipSnapshotAction::Snapshot(e.track);
			if (after != e.before)
				mUndo.Push(std::make_unique<ClipSnapshotAction>(mProject, e.track, e.before, std::move(after), mName));
		}
		mUndo.EndTransaction();
		mTracks.clear();
	}
private:
	struct TrackEntry {
		std::shared_ptr<Track> track;
		std::vector<ClipSnapshotAction::Entry> before;
	};

	Project* mProject;
	UndoManager& mUndo;
	const char* mName;
	std::vector<TrackEntry> mTracks;
};

// ---------------------------------------------------------------------------
// audio-clip warp/pitch edit (warp toggle, mode, segment bpm, transpose, plus
// any duration/offset the edit clamped). Snapshots the whole warp state before
// and after; the retained AudioClip shared_ptr keeps it alive across history
// Undo/Redo lock the project mutex because the audio thread reads these fields
// ---------------------------------------------------------------------------
class AudioClipWarpAction : public UndoableAction {
public:
	AudioClipWarpAction(Project* project, std::shared_ptr<AudioClip> clip,
						AudioClipWarpState before, AudioClipWarpState after, const char* name)
		: mProject(project), mClip(std::move(clip)), mBefore(before), mAfter(after), mName(name) {}

	void Undo() override { Apply(mBefore); }
	void Redo() override { Apply(mAfter); }
	const char* Name() const override { return mName; }
private:
	void Apply(const AudioClipWarpState& state) {
		if (!mClip)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		mClip->ApplyWarpState(state);
	}
	Project* mProject;
	std::shared_ptr<AudioClip> mClip;
	AudioClipWarpState mBefore;
	AudioClipWarpState mAfter;
	const char* mName;
};

// ---------------------------------------------------------------------------
// reversing an audio clip. the flip is materialized into the clip's samples, so
// the operation is its own inverse and both directions simply run it again.
// what is NOT symmetric is the window mirrored onto the flipped file: the clip's
// reach can have moved since (a tempo change, a transpose undone after this
// step), so the offset is snapshotted on both sides and pinned outright rather
// than re-derived. Undo/Redo lock because the audio thread reads the buffer
// ---------------------------------------------------------------------------
class ReverseClipAction : public UndoableAction {
public:
	ReverseClipAction(Project* project, std::shared_ptr<AudioClip> clip,
					  double beforeOffset, double afterOffset)
		: mProject(project), mClip(std::move(clip)), mBeforeOffset(beforeOffset), mAfterOffset(afterOffset) {}

	void Undo() override { Apply(mBeforeOffset); }
	void Redo() override { Apply(mAfterOffset); }
	const char* Name() const override { return "Reverse clip"; }
private:
	void Apply(double offset) {
		if (!mProject || !mClip)
			return;
		std::lock_guard<std::mutex> lock(mProject->GetMutex());
		mClip->Reverse(mProject->GetTransport().GetBpm());
		mClip->SetOffset(offset);
	}

	Project* mProject;
	std::shared_ptr<AudioClip> mClip;
	double mBeforeOffset;
	double mAfterOffset;
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
// piano-roll note edit (add / delete / move / resize / nudge / velocity)
// snapshots the whole note list before and after; the retained MIDIClip
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
