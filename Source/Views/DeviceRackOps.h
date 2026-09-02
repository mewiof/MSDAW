#pragma once
#include "EditorContext.h"
#include "ProcessorHost.h"
#include "Processors/RackProcessor.h"
#include "Track.h"
#include <functional>
#include <memory>
#include <vector>

class Project;

// ================================================================
// DEVICE RACK OPERATIONS
// ================================================================
// everything that acts on devices as a block: selection, grouping into a rack, and the
// edits the rack view, its context menus and its keyboard shortcuts all reach through.
// the rack draws pixels and nothing else, so "what Ctrl+G does" can be driven headlessly
//
// a device is never addressed by a bare index: a chain lives at a path (which track,
// then one device/chain hop per rack it descends through), and a selected device is held
// by shared_ptr and located by searching. both survive the reordering that a device edit
// is constantly doing to the indices around it
//
// every entry point below is responsible for its own project lock and for pushing one
// undo step covering every chain it touched
namespace DeviceRackOps {

	// how deep racks may nest. a drag payload has to be a fixed-size POD, and past four
	// levels the strip has no room to draw anything anyway
	constexpr int kMaxRackDepth = 4;

	// the address of one chain: the track it hangs under, then a (device, chain) hop for
	// every rack the path descends into. depth 0 is the track's own device chain
	struct ChainPath {
		int trackIndex = 0; // -1 is the master track
		int depth = 0;
		struct Hop {
			int device = 0;
			int chain = 0;
		};
		Hop hops[kMaxRackDepth] = {};
	};

	// the address of one device: a chain, and a slot in it
	struct DevicePath {
		ChainPath chain;
		int device = -1;
	};

	// where a device sits right now. a null host means it is no longer on the track
	struct DeviceLocation {
		std::shared_ptr<ProcessorHost> host;
		int index = -1;

		bool IsValid() const { return host != nullptr && index >= 0; }
	};

	// ---- addressing ----

	std::shared_ptr<Track> ResolveTrack(Project* project, int trackIndex);
	std::shared_ptr<ProcessorHost> ResolveChain(Project* project, const ChainPath& path);
	DeviceLocation ResolveDevice(Project* project, const DevicePath& path);

	// the path of the chain reached by descending into the rack in slot `deviceIndex`.
	// returns false when that would nest deeper than a rack is allowed to
	bool DescendPath(const ChainPath& parent, int deviceIndex, int chainIndex, ChainPath& out);

	// where `device` is under `track`, searching every rack chain below it
	DeviceLocation Locate(const std::shared_ptr<Track>& track, const std::shared_ptr<AudioProcessor>& device);

	// true when `host` is one of the chains inside `device` (or inside a rack inside it).
	// dropping a rack into its own chain would build a cycle the audio thread walks
	// forever, so every move asks this first
	bool HostIsInsideDevice(const std::shared_ptr<ProcessorHost>& host, const std::shared_ptr<AudioProcessor>& device);

	// ---- selection ----
	// the selection lives on EditorState and nowhere else, so these take it directly:
	// an edit below never touches it, and hands back what it made instead

	// drops selection entries whose device has left the track (an undo, a delete from
	// somewhere else). without this a stale device keeps a command alive that has
	// nothing left to act on
	void PruneSelection(EditorState& state, const std::shared_ptr<Track>& track);

	// which slots of `host` those devices occupy, ascending
	std::vector<int> IndicesIn(const std::shared_ptr<ProcessorHost>& host,
							   const std::vector<std::shared_ptr<AudioProcessor>>& devices);

	// shift-click: everything between the focused device and the clicked one, when both
	// are in the same chain. across chains it falls back to an exclusive select
	void SelectRangeTo(EditorState& state, const std::shared_ptr<Track>& track,
					   const std::shared_ptr<AudioProcessor>& target);

	// ---- edits ----

	void InsertDevice(Project* project, UndoManager& undoManager, const std::shared_ptr<ProcessorHost>& host,
					  int index, const std::shared_ptr<AudioProcessor>& device, const char* name);

	void MoveDevice(Project* project, UndoManager& undoManager,
					const std::shared_ptr<ProcessorHost>& sourceHost, int sourceIndex,
					const std::shared_ptr<ProcessorHost>& targetHost, int targetIndex);

	void RemoveDevices(Project* project, UndoManager& undoManager, const std::shared_ptr<Track>& track,
					   const std::vector<std::shared_ptr<AudioProcessor>>& devices);

	// flips every device to the same state, so a mixed selection resolves to one press
	// rather than half of it toggling each way
	void ToggleDevicesBypassed(Project* project, UndoManager& undoManager,
							   const std::vector<std::shared_ptr<AudioProcessor>>& devices);

	std::shared_ptr<AudioProcessor> DuplicateDevice(Project* project, UndoManager& undoManager,
													const std::shared_ptr<Track>& track,
													const std::shared_ptr<AudioProcessor>& device);

	// the devices are lifted into one chain of a new rack, which takes the slot the
	// first of them held. devices living in some other chain are left alone: a rack is
	// one chain's worth of devices, so grouping across chains has no meaning
	std::shared_ptr<RackProcessor> GroupDevices(Project* project, UndoManager& undoManager,
												const std::shared_ptr<Track>& track,
												const std::vector<std::shared_ptr<AudioProcessor>>& devices);

	// only a single-chain rack can be flattened back into the chain around it: two
	// chains in parallel have no serial arrangement that sounds the same
	bool CanUngroup(const std::shared_ptr<RackProcessor>& rack);
	// returns the devices that were let out, in the order they landed
	std::vector<std::shared_ptr<AudioProcessor>> UngroupRack(Project* project, UndoManager& undoManager,
															 const std::shared_ptr<Track>& track,
															 const std::shared_ptr<RackProcessor>& rack);

	// runs a rack-level edit (rename, recolor, a macro mapping, a chain added or
	// removed) under the project lock and records it as one history entry
	void EditRack(Project* project, UndoManager& undoManager, const std::shared_ptr<RackProcessor>& rack,
				  const char* name, const std::function<void()>& edit);

} // namespace DeviceRackOps
