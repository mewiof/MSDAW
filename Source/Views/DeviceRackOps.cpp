#include "PrecompHeader.h"
#include "DeviceRackOps.h"

#include "EditorContext.h"
#include "ProcessorIO.h"
#include "Project.h"
#include "Undo/Actions.h"

#include <algorithm>
#include <mutex>

namespace DeviceRackOps {

	namespace {

		// the rack in this slot, or null when the device there is an ordinary one
		std::shared_ptr<RackProcessor> AsRack(const std::shared_ptr<AudioProcessor>& device) {
			return std::dynamic_pointer_cast<RackProcessor>(device);
		}

		// every chain under `host`, the host itself first, in the order they draw
		void CollectHosts(const std::shared_ptr<ProcessorHost>& host, std::vector<std::shared_ptr<ProcessorHost>>& out) {
			if (!host)
				return;
			out.push_back(host);
			for (auto& device : host->GetProcessors()) {
				if (auto rack = AsRack(device)) {
					for (auto& chain : rack->GetChains())
						CollectHosts(chain, out);
				}
			}
		}

		// the rate a freshly added device has to be prepared at, or 0 when the engine
		// has not started a stream yet
		double CurrentSampleRate(Project* project) {
			return project ? project->GetTransport().GetSampleRate() : 0.0;
		}

	} // namespace

	// ================================================================
	// ADDRESSING
	// ================================================================

	std::shared_ptr<Track> ResolveTrack(Project* project, int trackIndex) {
		if (!project)
			return nullptr;
		if (trackIndex == -1)
			return project->GetMasterTrack();
		auto& tracks = project->GetTracks();
		if (trackIndex < 0 || trackIndex >= (int)tracks.size())
			return nullptr;
		return tracks[trackIndex];
	}

	std::shared_ptr<ProcessorHost> ResolveChain(Project* project, const ChainPath& path) {
		auto track = ResolveTrack(project, path.trackIndex);
		if (!track)
			return nullptr;

		std::shared_ptr<ProcessorHost> host = track;
		for (int i = 0; i < path.depth && i < kMaxRackDepth; ++i) {
			auto& devices = host->GetProcessors();
			const int deviceIndex = path.hops[i].device;
			if (deviceIndex < 0 || deviceIndex >= (int)devices.size())
				return nullptr;
			auto rack = AsRack(devices[deviceIndex]);
			if (!rack)
				return nullptr;
			const int chainIndex = path.hops[i].chain;
			if (chainIndex < 0 || chainIndex >= (int)rack->GetChains().size())
				return nullptr;
			host = rack->GetChains()[chainIndex];
		}
		return host;
	}

	DeviceLocation ResolveDevice(Project* project, const DevicePath& path) {
		DeviceLocation location;
		location.host = ResolveChain(project, path.chain);
		if (!location.host)
			return {};
		if (path.device < 0 || path.device >= (int)location.host->GetProcessors().size())
			return {};
		location.index = path.device;
		return location;
	}

	bool DescendPath(const ChainPath& parent, int deviceIndex, int chainIndex, ChainPath& out) {
		if (parent.depth >= kMaxRackDepth)
			return false;
		out = parent;
		out.hops[out.depth].device = deviceIndex;
		out.hops[out.depth].chain = chainIndex;
		out.depth++;
		return true;
	}

	DeviceLocation Locate(const std::shared_ptr<Track>& track, const std::shared_ptr<AudioProcessor>& device) {
		if (!track || !device)
			return {};

		std::vector<std::shared_ptr<ProcessorHost>> hosts;
		CollectHosts(track, hosts);

		for (auto& host : hosts) {
			auto& devices = host->GetProcessors();
			for (int i = 0; i < (int)devices.size(); ++i) {
				if (devices[i] == device)
					return {host, i};
			}
		}
		return {};
	}

	bool HostIsInsideDevice(const std::shared_ptr<ProcessorHost>& host, const std::shared_ptr<AudioProcessor>& device) {
		auto rack = AsRack(device);
		if (!rack || !host)
			return false;

		for (auto& chain : rack->GetChains()) {
			if (chain == host)
				return true;
			for (auto& nested : chain->GetProcessors()) {
				if (HostIsInsideDevice(host, nested))
					return true;
			}
		}
		return false;
	}

	// ================================================================
	// SELECTION
	// ================================================================

	void PruneSelection(EditorState& state, const std::shared_ptr<Track>& track) {
		auto& selection = state.selectedDevices;
		auto stale = std::remove_if(selection.begin(), selection.end(),
									[&](const std::shared_ptr<AudioProcessor>& device) {
										return !Locate(track, device).IsValid();
									});
		if (stale == selection.end())
			return;

		selection.erase(stale, selection.end());
		if (!state.IsDeviceSelected(state.selectedDevice))
			state.selectedDevice = selection.empty() ? nullptr : selection.back();
	}

	std::vector<int> IndicesIn(const std::shared_ptr<ProcessorHost>& host,
							   const std::vector<std::shared_ptr<AudioProcessor>>& devices) {
		std::vector<int> indices;
		if (!host)
			return indices;

		auto& hosted = host->GetProcessors();
		for (int i = 0; i < (int)hosted.size(); ++i) {
			if (std::find(devices.begin(), devices.end(), hosted[i]) != devices.end())
				indices.push_back(i);
		}
		return indices;
	}

	void SelectRangeTo(EditorState& state, const std::shared_ptr<Track>& track,
					   const std::shared_ptr<AudioProcessor>& target) {
		DeviceLocation to = Locate(track, target);
		DeviceLocation from = Locate(track, state.selectedDevice);

		if (!to.IsValid() || !from.IsValid() || from.host != to.host) {
			state.SelectDevice(target);
			return;
		}

		const int low = std::min(from.index, to.index);
		const int high = std::max(from.index, to.index);
		auto& devices = to.host->GetProcessors();

		std::vector<std::shared_ptr<AudioProcessor>> range;
		for (int i = low; i <= high; ++i)
			range.push_back(devices[i]);
		state.SetDeviceSelection(std::move(range), target);
	}

	// ================================================================
	// EDITS
	// ================================================================

	void InsertDevice(Project* project, UndoManager& undoManager, const std::shared_ptr<ProcessorHost>& host,
					  int index, const std::shared_ptr<AudioProcessor>& device, const char* name) {
		if (!project || !host || !device)
			return;

		DeviceEditScope scope(project, undoManager, name);
		scope.Touch(host);
		{
			std::lock_guard<std::mutex> lock(project->GetMutex());
			host->InsertProcessor(index, device);
			const double sampleRate = CurrentSampleRate(project);
			if (sampleRate > 0.0)
				device->PrepareToPlay(sampleRate);
		}
		scope.Commit();
	}

	void MoveDevice(Project* project, UndoManager& undoManager,
					const std::shared_ptr<ProcessorHost>& sourceHost, int sourceIndex,
					const std::shared_ptr<ProcessorHost>& targetHost, int targetIndex) {
		if (!project || !sourceHost || !targetHost)
			return;
		auto& sourceDevices = sourceHost->GetProcessors();
		if (sourceIndex < 0 || sourceIndex >= (int)sourceDevices.size())
			return;

		auto device = sourceDevices[sourceIndex];
		// a rack cannot be dropped into itself, and putting a device back exactly where
		// it already is is not an edit
		if (HostIsInsideDevice(targetHost, device))
			return;
		if (sourceHost == targetHost && (targetIndex == sourceIndex || targetIndex == sourceIndex + 1))
			return;

		DeviceEditScope scope(project, undoManager, "Move device");
		scope.Touch(sourceHost);
		scope.Touch(targetHost);
		{
			std::lock_guard<std::mutex> lock(project->GetMutex());
			if (sourceHost == targetHost) {
				sourceHost->MoveProcessor(sourceIndex, targetIndex);
			} else {
				sourceHost->RemoveProcessor(sourceIndex);
				targetHost->InsertProcessor(targetIndex, device);
			}
		}
		scope.Commit();
	}

	void RemoveDevices(Project* project, UndoManager& undoManager, const std::shared_ptr<Track>& track,
					   const std::vector<std::shared_ptr<AudioProcessor>>& devices) {
		if (!project || !track || devices.empty())
			return;

		DeviceEditScope scope(project, undoManager, devices.size() > 1 ? "Delete devices" : "Delete device");
		for (const auto& device : devices) {
			DeviceLocation location = Locate(track, device);
			if (location.IsValid())
				scope.Touch(location.host);
		}
		{
			std::lock_guard<std::mutex> lock(project->GetMutex());
			for (const auto& device : devices) {
				// located again inside the lock: every removal shifts the slots after
				// it, and a selection can span several chains
				DeviceLocation location = Locate(track, device);
				if (location.IsValid())
					location.host->RemoveProcessor(location.index);
			}
		}
		scope.Commit();
	}

	void ToggleDevicesBypassed(Project* project, UndoManager& undoManager,
							   const std::vector<std::shared_ptr<AudioProcessor>>& devices) {
		if (!project || devices.empty())
			return;

		// a mixed selection deactivates: what the user sees is "some of these are on",
		// and one press is expected to end that
		const bool bypassed = std::any_of(devices.begin(), devices.end(),
										  [](const std::shared_ptr<AudioProcessor>& device) { return !device->IsBypassed(); });

		std::vector<DeviceBypassAction::Entry> before;
		std::vector<DeviceBypassAction::Entry> after;
		for (const auto& device : devices) {
			before.push_back({device, device->IsBypassed()});
			after.push_back({device, bypassed});
		}

		{
			std::lock_guard<std::mutex> lock(project->GetMutex());
			for (const auto& device : devices)
				device->SetBypassed(bypassed);
		}
		undoManager.Push(std::make_unique<DeviceBypassAction>(
			project, std::move(before), std::move(after),
			bypassed ? "Deactivate device" : "Activate device"));
	}

	std::shared_ptr<AudioProcessor> DuplicateDevice(Project* project, UndoManager& undoManager,
													const std::shared_ptr<Track>& track,
													const std::shared_ptr<AudioProcessor>& device) {
		DeviceLocation location = Locate(track, device);
		if (!location.IsValid())
			return nullptr;

		auto clone = ProcessorIO::CloneProcessor(device);
		if (!clone)
			return nullptr;

		InsertDevice(project, undoManager, location.host, location.index + 1, clone, "Duplicate device");
		return clone;
	}

	std::shared_ptr<RackProcessor> GroupDevices(Project* project, UndoManager& undoManager,
												const std::shared_ptr<Track>& track,
												const std::vector<std::shared_ptr<AudioProcessor>>& devices) {
		if (!project || !track || devices.empty())
			return nullptr;

		// the group happens in the chain holding the first of them; anything living
		// elsewhere is not part of this rack
		DeviceLocation anchor = Locate(track, devices.front());
		if (!anchor.IsValid())
			return nullptr;

		std::vector<int> indices = IndicesIn(anchor.host, devices);
		if (indices.empty())
			return nullptr;

		auto& hostDevices = anchor.host->GetProcessors();
		std::vector<std::shared_ptr<AudioProcessor>> grouped;
		bool holdsInstrument = false;
		for (int index : indices) {
			grouped.push_back(hostDevices[index]);
			holdsInstrument = holdsInstrument || hostDevices[index]->IsInstrument();
		}

		auto rack = std::make_shared<RackProcessor>();
		rack->SetName(holdsInstrument ? "Instrument Rack" : "Audio Effect Rack");
		auto chain = rack->AddChain("Chain");

		DeviceEditScope scope(project, undoManager, "Group devices");
		scope.Touch(anchor.host);
		{
			std::lock_guard<std::mutex> lock(project->GetMutex());
			// back to front, so each removal leaves the slots before it alone; the rack
			// lands where the first of them was
			for (auto index = indices.rbegin(); index != indices.rend(); ++index)
				anchor.host->RemoveProcessor(*index);
			for (auto& device : grouped)
				chain->AddProcessor(device);
			anchor.host->InsertProcessor(indices.front(), rack);

			const double sampleRate = CurrentSampleRate(project);
			if (sampleRate > 0.0)
				rack->PrepareToPlay(sampleRate);
		}
		scope.Commit();

		return rack;
	}

	bool CanUngroup(const std::shared_ptr<RackProcessor>& rack) {
		return rack && rack->GetChains().size() <= 1;
	}

	std::vector<std::shared_ptr<AudioProcessor>> UngroupRack(Project* project, UndoManager& undoManager,
															 const std::shared_ptr<Track>& track,
															 const std::shared_ptr<RackProcessor>& rack) {
		if (!project || !track || !CanUngroup(rack))
			return {};

		DeviceLocation location = Locate(track, rack);
		if (!location.IsValid())
			return {};

		std::vector<std::shared_ptr<AudioProcessor>> released;
		if (!rack->GetChains().empty())
			released = rack->GetChains().front()->GetProcessors();

		DeviceEditScope scope(project, undoManager, "Ungroup rack");
		scope.Touch(location.host);
		{
			std::lock_guard<std::mutex> lock(project->GetMutex());
			location.host->RemoveProcessor(location.index);
			for (int i = 0; i < (int)released.size(); ++i)
				location.host->InsertProcessor(location.index + i, released[i]);
		}
		scope.Commit();

		return released;
	}

	void EditRack(Project* project, UndoManager& undoManager, const std::shared_ptr<RackProcessor>& rack,
				  const char* name, const std::function<void()>& edit) {
		if (!project || !rack || !edit)
			return;

		RackProcessor::RackState before = rack->CaptureState();
		{
			std::lock_guard<std::mutex> lock(project->GetMutex());
			edit();
		}
		undoManager.Push(std::make_unique<RackStateAction>(project, rack, std::move(before), rack->CaptureState(), name));
	}

} // namespace DeviceRackOps
