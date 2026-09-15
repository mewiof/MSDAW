#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class AudioProcessor;
class Parameter;

// an ordered chain of devices with an owner: a track, or one chain of a rack nested
// inside one. everything that walks or edits a track's devices - the rack view and its
// drag targets, the undo actions, automation collection - goes through this seam, so a
// device buried three racks deep is reached by exactly the same code as one sitting
// straight on the track
//
// a host is always held by shared_ptr (Track and RackChain both are), which is what
// lets an undo action keep the chain it edits alive across the whole history
//
// NOTE: membership changes are structural. every caller mutating a chain holds
// Project::mMutex, the same rule the track's own processor list has always had
class ProcessorHost {
public:
	virtual ~ProcessorHost() = default;

	virtual std::vector<std::shared_ptr<AudioProcessor>>& GetProcessors() = 0;

	void AddProcessor(std::shared_ptr<AudioProcessor> processor);
	void InsertProcessor(int index, std::shared_ptr<AudioProcessor> processor);
	void RemoveProcessor(int index);
	// insert-before semantics: toIndex names a slot in the list as it stands, so moving
	// 0 -> 2 in [a,b,c] lands a between b and c
	void MoveProcessor(int fromIndex, int toIndex);
	// undo support: replace the whole chain at once
	void SetProcessors(std::vector<std::shared_ptr<AudioProcessor>> processors);

	// every parameter reachable from this chain: the host's own, then each device's,
	// then recursively everything inside a device that hosts chains of its own. this is
	// what keeps a device automatable after it has been grouped into a rack
	void CollectParameters(std::vector<Parameter*>& out);

	// the same walk, narrowed to what the device panels actually show: a configured
	// device contributes the parameters on its panel, an unconfigured one contributes
	// all of them unless it has an editor of its own to configure it from. what the
	// automation lane offers, so a plugin publishing thousands of parameters does not
	// bury the handful worth automating
	void CollectPanelParameters(std::vector<Parameter*>& out);

	// bumped by every membership change in any chain in the session. a rack's macro
	// mappings cache the Parameter* they drive, and re-resolve when they see a newer
	// value - so a device moved, deleted, or brought back by an undo anywhere in the
	// project can never leave a macro writing into a parameter that has left the rack.
	// one counter for everything keeps the audio thread's check at a single load
	static uint32_t ChainGeneration() { return sChainGeneration.load(std::memory_order_relaxed); }
	static void BumpChainGeneration() { sChainGeneration.fetch_add(1, std::memory_order_relaxed); }
protected:
	// parameters belonging to the host itself rather than to a device in it (a track's
	// volume/pan, a rack chain's mixer). the base has none
	virtual void CollectOwnParameters(std::vector<Parameter*>& out) { (void)out; }
private:
	static std::atomic<uint32_t> sChainGeneration;
};
