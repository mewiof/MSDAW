#pragma once
#include "EditorContext.h"
#include "Processors/ModulatorProcessor.h"
#include "Views/DeviceRackOps.h"
#include "imgui.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RackChain;
class RackProcessor;

// the device strip: the selected track's chain, drawn left to right, with a rack's own
// chains drawn inside it by the same code one level down
//
// nothing here mutates the graph while it is drawing. a click that adds, removes, moves
// or groups a device queues the edit and the whole strip finishes the frame first, which
// is what keeps the recursion from walking a chain that has changed under it
class DeviceRackView {
public:
	DeviceRackView(EditorContext& context)
		: mContext(context) {}
	void Render(const ImVec2& pos, float width, float height);
private:
	// what the rename popup is currently aimed at
	enum class RenameTarget {
		None,
		Rack,
		Macro,
		Chain
	};

	// the three panels a rack lays out side by side, and what they cost in width. one
	// place computes them, so the shell the rack is drawn in is exactly as wide as what
	// goes inside it
	struct RackLayout {
		float viewColumn = 0.0f;
		float macros = 0.0f;
		float chains = 0.0f;
		float devices = 0.0f;
		float total = 0.0f;
	};

	// ---- measurement ----
	float DeviceWidth(const std::shared_ptr<AudioProcessor>& device) const;
	float ChainWidth(const std::shared_ptr<ProcessorHost>& host) const;
	RackLayout ComputeRackLayout(const std::shared_ptr<RackProcessor>& rack) const;

	// ---- drawing ----
	void RenderChain(const std::shared_ptr<ProcessorHost>& host, const DeviceRackOps::ChainPath& path, float bodyHeight);
	void RenderDevice(const std::shared_ptr<ProcessorHost>& host, const DeviceRackOps::ChainPath& path, int index, float bodyHeight);
	void RenderRackBody(const std::shared_ptr<RackProcessor>& rack, const DeviceRackOps::ChainPath& path, int deviceIndex);
	void RenderModulatorBody(const std::shared_ptr<ModulatorProcessor>& modulator);
	void RenderMacroPanel(const std::shared_ptr<RackProcessor>& rack, float width, float height);
	void RenderChainList(const std::shared_ptr<RackProcessor>& rack, const DeviceRackOps::ChainPath& path, int deviceIndex, float width, float height);
	void RenderMappingBrowser(const std::shared_ptr<RackProcessor>& rack);
	void RenderTargetBrowser(const std::shared_ptr<ModulatorProcessor>& modulator);
	void RenderParameterPanel(const std::shared_ptr<AudioProcessor>& device);
	void RenderParameterList(const std::vector<std::unique_ptr<Parameter>>& parameters);
	void RenderRenamePopup();
	void RenderDeviceContextMenu(const std::shared_ptr<AudioProcessor>& device);

	// ---- gestures ----
	// the drop targets between and around devices, and the chain rows a device can be
	// dragged onto. true when something was accepted and the chain must not be walked
	// any further this frame
	bool AcceptDeviceDrop(const std::shared_ptr<ProcessorHost>& host, const DeviceRackOps::ChainPath& path, int index);
	// the commands that also move the selection: the operations themselves never touch
	// it, they hand back what they made
	void DeleteDevices(const std::vector<std::shared_ptr<AudioProcessor>>& devices);
	void GroupDevices(const std::vector<std::shared_ptr<AudioProcessor>>& devices);
	void UngroupRack(const std::shared_ptr<RackProcessor>& rack);
	void ClickDevice(const std::shared_ptr<AudioProcessor>& device);
	void HandleShortcuts();
	void OpenRename(RenameTarget target, const std::shared_ptr<RackProcessor>& rack, int index, const std::string& current);

	// the devices a command should act on: the selection when the clicked device is part
	// of it, otherwise just that device
	std::vector<std::shared_ptr<AudioProcessor>> CommandTargets(const std::shared_ptr<AudioProcessor>& device) const;

	EditorContext& mContext;

	// the frame's track and the edits queued against it, both valid only while Render
	// is on the stack
	std::shared_ptr<Track> mTrack;
	std::vector<std::function<void()>> mDeferred;

	// the rack whose macros are taking mappings, and the one whose mapping browser is
	// open. weak, because the rack can be deleted while either is up
	std::weak_ptr<RackProcessor> mMapModeRack;

	// the device currently configuring its panel, and the list it had when Add was
	// switched on. the whole session - however many controls get touched in the
	// plugin's editor - closes as one history entry when Add goes off again
	std::weak_ptr<AudioProcessor> mPanelCaptureDevice;
	std::vector<int> mPanelCaptureBefore;
	bool mPanelCaptureSeen = false;

	// the one device listing every parameter it publishes rather than its panel. a
	// look at the full list, not a configuration of it, so it is not saved with the
	// device and only one device is ever expanded at a time
	std::weak_ptr<AudioProcessor> mPanelShowAllDevice;
	void BeginPanelCapture(const std::shared_ptr<AudioProcessor>& device);
	void EndPanelCapture();

	// the parameter a Map press will hand to a macro. it is remembered rather than read
	// live from Parameter::GetSelectedParameter(): clicking the Map button is itself a
	// click that lands on no parameter, and the parameter selection is dropped on that
	// press - one frame before the button reports it was pressed. only ever compared
	// against the parameters of live devices, never dereferenced, and dropped as soon as
	// an edit could have taken the device it belongs to away
	Parameter* mMapCandidate = nullptr;
	std::weak_ptr<RackProcessor> mBrowserRack;
	bool mOpenBrowserPopup = false;

	// the same three pieces of state for a modulator. it maps the same way a rack macro
	// does - arm Map, click a parameter, hand it over - except that what it may reach is
	// the whole project rather than the inside of one device
	std::weak_ptr<ModulatorProcessor> mMapModeModulator;
	std::weak_ptr<ModulatorProcessor> mBrowserModulator;
	bool mOpenTargetPopup = false;

	// a min/max drag in the mapping browser is one undo entry, not one per frame: the
	// state is captured when the drag starts and pushed when it ends
	std::weak_ptr<RackProcessor> mRangeEditRack;
	RackProcessor::RackState mRangeEditBefore;
	std::weak_ptr<ModulatorProcessor> mRangeEditModulator;
	ModulatorProcessor::State mRangeEditModulatorBefore;

	RenameTarget mRenameTarget = RenameTarget::None;
	std::weak_ptr<RackProcessor> mRenameRack;
	int mRenameIndex = -1;
	bool mOpenRenamePopup = false;
	char mRenameBuffer[64] = {};
};
