#include "PrecompHeader.h"
#include "DeviceRackView.h"
#include "AppConfig.h"
#include "DeviceRackOps.h"
#include "Parameters/KnobParameter.h"
#include "ProcessorFactory.h"
#include "ProcessorIO.h"
#include "Processors/ModulatorProcessor.h"
#include "Processors/RackProcessor.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"
#include "Project.h"
#include "Theme.h"
#include "Track.h"
#include "Undo/Actions.h"
#include <algorithm>
#include <cstring>
#include <mutex>

using DeviceRackOps::ChainPath;
using DeviceRackOps::DevicePath;

namespace {

	// ---- layout, in unscaled pixels ----
	constexpr float kDropZoneWidth = 10.0f;
	constexpr float kDeviceWidth = 280.0f;
	constexpr float kDeviceWidthSidechain = 430.0f; // source picker + graph + a full knob row
	constexpr float kDeviceWidthEQ = 620.0f;		// knob column + graph + globals, over eight bands
	constexpr float kDeviceWidthModulator = 500.0f; // two lane panels wide enough for sixteen steps
	constexpr float kMacroCellWidth = 66.0f;
	constexpr float kChainListWidth = 176.0f;
	constexpr float kViewColumnWidth = 20.0f;
	constexpr float kMinRackDevicesWidth = 160.0f; // room for the "drop devices here" hint
	constexpr float kMinRackWidth = 210.0f;		   // the rack's own header row, panels or not
	constexpr float kHeaderHeight = 25.0f;

	// the rack a device is, or null
	std::shared_ptr<RackProcessor> AsRack(const std::shared_ptr<AudioProcessor>& device) {
		return std::dynamic_pointer_cast<RackProcessor>(device);
	}

	// the modulator a device is, or null
	std::shared_ptr<ModulatorProcessor> AsModulator(const std::shared_ptr<AudioProcessor>& device) {
		return std::dynamic_pointer_cast<ModulatorProcessor>(device);
	}

	// a two-state toggle drawn as a button, used for the rack's view column and for a
	// chain's mute / solo
	bool ToggleButton(const char* label, bool active, const ImVec2& size, ImU32 activeColor) {
		const Theme& th = Theme::Instance();
		if (active) {
			ImGui::PushStyleColor(ImGuiCol_Button, activeColor);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeColor);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}
		const bool pressed = ImGui::Button(label, size);
		if (active)
			ImGui::PopStyleColor(3);
		return pressed;
	}

	// the curated palette, offered as swatches beside a free picker so a rack, a chain
	// or a macro can be colored to match a track without hunting for the value
	bool ColorMenu(const char* id, ImU32& color) {
		const Theme& th = Theme::Instance();
		bool changed = false;

		ImGui::PushID(id);
		for (int i = 0; i < 12; ++i) {
			const ImU32 swatch = th.TrackColor(i);
			ImGui::PushID(i);
			if (ImGui::ColorButton("##Swatch", ImGui::ColorConvertU32ToFloat4(swatch), ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18))) {
				color = swatch;
				changed = true;
			}
			ImGui::PopID();
			if (i % 6 != 5)
				ImGui::SameLine();
		}

		ImVec4 picked = ImGui::ColorConvertU32ToFloat4(color);
		if (ImGui::ColorPicker4("##Picker", (float*)&picked,
								ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_NoInputs)) {
			color = ImGui::ColorConvertFloat4ToU32(picked);
			changed = true;
		}
		if (ImGui::MenuItem("Reset to default")) {
			color = 0;
			changed = true;
		}
		ImGui::PopID();
		return changed;
	}

} // namespace

// ================================================================
// MEASUREMENT
// ================================================================

float DeviceRackView::DeviceWidth(const std::shared_ptr<AudioProcessor>& device) const {
	const float scale = mContext.state.mainScale;
	if (auto rack = AsRack(device))
		return ComputeRackLayout(rack).total;

	const std::string processorId = device->GetProcessorId();
	if (processorId == "AutoSidechain")
		return kDeviceWidthSidechain * scale;
	if (processorId == "EQEight")
		return kDeviceWidthEQ * scale;
	if (processorId == "Modulator")
		return kDeviceWidthModulator * scale;
	return kDeviceWidth * scale;
}

float DeviceRackView::ChainWidth(const std::shared_ptr<ProcessorHost>& host) const {
	const float scale = mContext.state.mainScale;
	const float spacing = ImGui::GetStyle().ItemSpacing.x;

	float width = kDropZoneWidth * scale; // the drop slot before the first device
	for (auto& device : host->GetProcessors())
		width += spacing + DeviceWidth(device) + spacing + kDropZoneWidth * scale;
	return width;
}

DeviceRackView::RackLayout DeviceRackView::ComputeRackLayout(const std::shared_ptr<RackProcessor>& rack) const {
	const ImGuiStyle& style = ImGui::GetStyle();
	const float scale = mContext.state.mainScale;

	RackLayout layout;
	layout.viewColumn = kViewColumnWidth * scale;
	int columns = 1;

	if (rack->mShowMacros) {
		// two rows of knobs, however many macros are revealed - the same shape the
		// reference product lays eight or sixteen of them out in
		const int macroColumns = (rack->GetVisibleMacroCount() + 1) / 2;
		layout.macros = macroColumns * kMacroCellWidth * scale + (macroColumns - 1) * style.ItemSpacing.x;
		++columns;
	}
	if (rack->mShowChains) {
		layout.chains = kChainListWidth * scale;
		++columns;
	}
	if (rack->mShowDevices) {
		auto chain = rack->GetSelectedChain();
		layout.devices = std::max(chain ? ChainWidth(chain) : 0.0f, kMinRackDevicesWidth * scale);
		++columns;
	}

	layout.total = layout.viewColumn + layout.macros + layout.chains + layout.devices;
	layout.total += style.ItemSpacing.x * (columns - 1) + style.WindowPadding.x * 2.0f + 2.0f;
	layout.total = std::max(layout.total, kMinRackWidth * scale);
	return layout;
}

// ================================================================
// RENDER
// ================================================================

void DeviceRackView::Render(const ImVec2& pos, float width, float height) {
	const ImVec2 defaultPadding = ImGui::GetStyle().WindowPadding;
	// prevent double-padding issues with full-size children
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::Begin("Device Rack", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	Project* project = mContext.GetProject();
	mTrack = DeviceRackOps::ResolveTrack(project, mContext.state.selectedTrackIndex);
	mDeferred.clear();

	if (mTrack) {
		// a device the selection names can have been deleted from somewhere else, or
		// carried off by an undo, since the last frame
		DeviceRackOps::PruneSelection(mContext.state, mTrack);
		HandleShortcuts();

		ImGui::BeginChild("DevicesArea", ImVec2(width, height), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		// every device is exactly as tall as the strip has room for, horizontal
		// scrollbar included. measured rather than assumed: handing the devices the
		// rack height is what pushed them past the bottom and grew a second, vertical
		// scrollbar over the whole row
		const float bodyHeight = std::max(ImGui::GetContentRegionAvail().y, 1.0f);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, defaultPadding);
		ChainPath path;
		path.trackIndex = mContext.state.selectedTrackIndex;
		RenderChain(mTrack, path, bodyHeight);
		ImGui::PopStyleVar();

		ImGui::EndChild();
	} else {
		ImGui::TextDisabled("No Track Selected. Select a track to view devices.");
	}

	RenderRenamePopup();

	ImGui::End();
	ImGui::PopStyleVar();

	// the strip has finished drawing, so the chains it walked can be edited safely
	const bool edited = !mDeferred.empty();
	for (auto& edit : mDeferred)
		edit();
	mDeferred.clear();
	mTrack = nullptr;

	// an edit can have taken the armed parameter's device out of the project
	if (edited || (mMapModeRack.expired() && mMapModeModulator.expired()))
		mMapCandidate = nullptr;
}

void DeviceRackView::RenderChain(const std::shared_ptr<ProcessorHost>& host, const ChainPath& path, float bodyHeight) {
	const Theme& th = Theme::Instance();
	const float scale = mContext.state.mainScale;
	auto& devices = host->GetProcessors();

	for (int i = 0; i <= (int)devices.size(); ++i) {
		ImGui::PushID(i * 2 + 1);

		float dropWidth = kDropZoneWidth * scale;
		// at the very end, extend the drop zone to fill whatever is left
		if (i == (int)devices.size()) {
			const float available = ImGui::GetContentRegionAvail().x;
			if (available > dropWidth)
				dropWidth = available;
		}

		ImGui::InvisibleButton("##DropZone", ImVec2(dropWidth, bodyHeight));
		const ImVec2 dropMin = ImGui::GetItemRectMin();
		const ImVec2 dropMax = ImGui::GetItemRectMax();
		if (AcceptDeviceDrop(host, path, i)) {
			ImGui::GetWindowDrawList()->AddLine(ImVec2(dropMin.x + dropWidth * 0.5f, dropMin.y),
												ImVec2(dropMin.x + dropWidth * 0.5f, dropMax.y), th.dropLine, 2.0f * scale);
		}
		if (i == (int)devices.size() && devices.empty()) {
			const char* hint = path.depth == 0 ? "Drag Instruments or Effects here from the Library"
											   : "Drop devices here";
			ImGui::GetWindowDrawList()->AddText(ImVec2(dropMin.x + 8.0f * scale, dropMin.y + bodyHeight * 0.5f), th.textDim, hint);
		}
		ImGui::PopID();

		if (i == (int)devices.size())
			break;

		ImGui::SameLine();
		RenderDevice(host, path, i, bodyHeight);
		ImGui::SameLine();
	}
}

void DeviceRackView::RenderDevice(const std::shared_ptr<ProcessorHost>& host, const ChainPath& path, int index, float bodyHeight) {
	auto device = host->GetProcessors()[index];
	auto rack = AsRack(device);
	const Theme& th = Theme::Instance();
	const ImGuiStyle& style = ImGui::GetStyle();
	const float scale = mContext.state.mainScale;
	const bool selected = mContext.state.IsDeviceSelected(device);

	ImU32 background = rack ? th.bgDeviceRack : (device->IsInstrument() ? th.bgDeviceInstrument : th.bgDeviceEffect);
	if (rack && rack->GetColor() != 0)
		background = rack->GetColor();
	if (device->IsBypassed())
		background = th.bgDeepest;

	const float width = DeviceWidth(device);

	ImGui::PushID(device.get());
	ImGui::PushStyleColor(ImGuiCol_ChildBg, background);

	ImGuiChildFlags childFlags = ImGuiChildFlags_Borders;
	// only a plain device sitting straight on the track may be widened by hand: a rack
	// is exactly as wide as the panels inside it, and a device inside one is a term in
	// that sum
	if (!rack && path.depth == 0)
		childFlags |= ImGuiChildFlags_ResizeX;

	ImGui::BeginChild("DeviceBody", ImVec2(width, bodyHeight), childFlags, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	// the selection outline is drawn from inside the device, over the top of its own
	// border: a child window paints its frame after the parent has drawn, so an outline
	// laid down out there comes back with its edges painted over
	if (selected) {
		const ImVec2 bodyMin = ImGui::GetWindowPos();
		const ImVec2 bodySize = ImGui::GetWindowSize();
		ImGui::GetWindowDrawList()->AddRect(ImVec2(bodyMin.x + 1.0f, bodyMin.y + 1.0f),
											ImVec2(bodyMin.x + bodySize.x - 1.0f, bodyMin.y + bodySize.y - 1.0f),
											th.accent, style.ChildRounding, 0, 2.0f * scale);
	}

	// ---- header ----
	ImGui::BeginGroup();

	const ImVec2 ledPos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##Activator", ImVec2(12.0f * scale, 12.0f * scale));
	if (ImGui::IsItemClicked()) {
		auto targets = CommandTargets(device);
		mDeferred.push_back([this, targets]() { DeviceRackOps::ToggleDevicesBypassed(mContext.GetProject(), mContext.undoManager, targets); });
	}
	ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(ledPos.x + 6.0f * scale, ledPos.y + 6.0f * scale),
											   5.0f * scale, device->IsBypassed() ? th.border : th.accent);

	ImGui::SameLine();
	if (selected)
		ImGui::PushStyleColor(ImGuiCol_Text, th.accent);
	ImGui::TextUnformatted(device->GetName());
	if (selected)
		ImGui::PopStyleColor();

	if (device->HasEditor()) {
		ImGui::SameLine();
		if (ImGui::SmallButton("Edit"))
			device->OpenEditor(mContext.nativeWindowHandle);
	}

	ImGui::EndGroup();
	const ImVec2 afterHeader = ImGui::GetCursorPos();

	// the whole header strip is the hit area for selecting and dragging the device. the
	// widgets above resolved their own clicks as they were drawn, so a click on the LED
	// both toggles it and selects the device it belongs to
	ImGui::SetCursorPos(ImVec2(0, 0));
	ImGui::InvisibleButton("##Header", ImVec2(width, kHeaderHeight * scale));
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		ClickDevice(device);
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected)
		mContext.state.SelectDevice(device);
	if (rack && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::IsItemHovered())
		OpenRename(RenameTarget::Rack, rack, -1, rack->GetName());

	if (ImGui::BeginDragDropSource()) {
		DevicePath payload;
		payload.chain = path;
		payload.device = index;
		ImGui::SetDragDropPayload("PROCESSOR_MOVE", &payload, sizeof(payload));
		ImGui::TextUnformatted(device->GetName());
		ImGui::EndDragDropSource();
	}
	RenderDeviceContextMenu(device);

	ImGui::SetCursorPos(afterHeader);
	ImGui::Separator();

	// ---- body ----
	// what is left once the header strip is out of the way, and the one height every
	// device lays itself out inside. nothing below this point scrolls
	const ImVec2 available = ImGui::GetContentRegionAvail();
	if (rack) {
		RenderRackBody(rack, path, index);
	} else if (auto modulator = AsModulator(device)) {
		RenderModulatorBody(modulator);
	} else if (device->IsBypassed()) {
		ImGui::TextDisabled("Device Bypassed");
	} else if (!device->RenderCustomUI(available)) {
		// a plugin with no editor of its own is drawn from its parameter list, and that
		// list is as long as the plugin says it is - a hundred sliders belong behind a
		// scrollbar, not spread across a device ten columns wide
		ImGui::BeginChild("ParamsScroll", ImVec2(0, 0));
		for (auto& parameter : device->GetParameters())
			parameter->Draw();
		ImGui::EndChild();
	}

	ImGui::EndChild();
	ImGui::PopStyleColor();

	ImGui::PopID();
}

// ================================================================
// RACK
// ================================================================

void DeviceRackView::RenderRackBody(const std::shared_ptr<RackProcessor>& rack, const ChainPath& path, int deviceIndex) {
	const Theme& th = Theme::Instance();
	const float scale = mContext.state.mainScale;
	const RackLayout layout = ComputeRackLayout(rack);

	// ---- rack header row ----
	const bool mapMode = (mMapModeRack.lock() == rack);
	if (mapMode) {
		if (Parameter* clicked = Parameter::GetSelectedParameter())
			mMapCandidate = clicked;
	}
	if (ToggleButton("Map", mapMode, ImVec2(0, 0), th.accent))
		mMapModeRack = mapMode ? std::weak_ptr<RackProcessor>() : std::weak_ptr<RackProcessor>(rack);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Click a device parameter, then Map on the macro that should drive it");

	ImGui::SameLine();
	if (ImGui::SmallButton("-")) {
		const int count = rack->GetVisibleMacroCount() - 1;
		mDeferred.push_back([this, rack, count]() {
			DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Macro count", [rack, count]() { rack->SetVisibleMacroCount(count); });
		});
	}
	ImGui::SameLine();
	ImGui::Text("%d", rack->GetVisibleMacroCount());
	ImGui::SameLine();
	if (ImGui::SmallButton("+")) {
		const int count = rack->GetVisibleMacroCount() + 1;
		mDeferred.push_back([this, rack, count]() {
			DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Macro count", [rack, count]() { rack->SetVisibleMacroCount(count); });
		});
	}

	if (mapMode) {
		// what a Map press would hand over, so the arming step is visible rather than
		// something the user has to remember doing
		ImGui::SameLine();
		if (mMapCandidate)
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th.accent), "-> %s", mMapCandidate->name.c_str());
		else
			ImGui::TextDisabled("click a parameter");
	}

	ImGui::SameLine();
	if (ImGui::SmallButton("Mappings")) {
		mBrowserRack = rack;
		mOpenBrowserPopup = true;
	}
	// the popup is opened here whoever asked for it: OpenPopup names an id relative to
	// the current stack, and a menu item asking from inside its own popup would name one
	// BeginPopup below never looks for
	if (mOpenBrowserPopup && mBrowserRack.lock() == rack) {
		ImGui::OpenPopup("MacroMappings");
		mOpenBrowserPopup = false;
	}
	RenderMappingBrowser(rack);

	const float columnHeight = std::max(ImGui::GetContentRegionAvail().y, 1.0f);

	// ---- view column ----
	ImGui::PushStyleColor(ImGuiCol_ChildBg, th.bgRackInner);
	ImGui::BeginChild("ViewColumn", ImVec2(layout.viewColumn, columnHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
	const ImVec2 toggleSize(layout.viewColumn - 2.0f * scale, columnHeight / 3.0f - 2.0f * scale);
	if (ToggleButton("M", rack->mShowMacros, toggleSize, th.accentMuted))
		rack->mShowMacros = !rack->mShowMacros;
	if (ToggleButton("C", rack->mShowChains, toggleSize, th.accentMuted))
		rack->mShowChains = !rack->mShowChains;
	if (ToggleButton("D", rack->mShowDevices, toggleSize, th.accentMuted))
		rack->mShowDevices = !rack->mShowDevices;
	ImGui::EndChild();
	ImGui::PopStyleColor();

	if (rack->mShowMacros) {
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_ChildBg, th.bgRackInner);
		ImGui::BeginChild("MacroPanel", ImVec2(layout.macros, columnHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
		RenderMacroPanel(rack, layout.macros, columnHeight);
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	if (rack->mShowChains) {
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_ChildBg, th.bgRackInner);
		ImGui::BeginChild("ChainList", ImVec2(layout.chains, columnHeight), ImGuiChildFlags_None, ImGuiWindowFlags_None);
		RenderChainList(rack, path, deviceIndex, layout.chains, columnHeight);
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	if (rack->mShowDevices) {
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_ChildBg, th.bgRackInner);
		ImGui::BeginChild("RackDevices", ImVec2(layout.devices, columnHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		auto chain = rack->GetSelectedChain();
		ChainPath childPath;
		if (!chain) {
			ImGui::TextDisabled("No chains");
		} else if (!DeviceRackOps::DescendPath(path, deviceIndex, rack->GetSelectedChainIndex(), childPath)) {
			ImGui::TextDisabled("Racks nest %d deep", DeviceRackOps::kMaxRackDepth);
		} else {
			RenderChain(chain, childPath, std::max(ImGui::GetContentRegionAvail().y, 1.0f));
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}
}

void DeviceRackView::RenderMacroPanel(const std::shared_ptr<RackProcessor>& rack, float width, float height) {
	const Theme& th = Theme::Instance();
	const ImGuiStyle& style = ImGui::GetStyle();
	const float scale = mContext.state.mainScale;
	const bool mapMode = (mMapModeRack.lock() == rack);

	const int count = rack->GetVisibleMacroCount();
	const int columns = (count + 1) / 2;
	const int rows = count > columns ? 2 : 1;
	// the gap SameLine puts between two cells comes out of the cells, not out of the
	// panel: leave it in and the last column hangs off the edge and is clipped
	const float cellWidth = (width - style.ItemSpacing.x * (columns - 1)) / (float)columns;
	const float cellHeight = height / (float)rows;

	const float titleHeight = ImGui::GetTextLineHeight();
	const float mapButtonHeight = mapMode ? ImGui::GetFrameHeight() + style.ItemSpacing.y : 0.0f;
	const float radius = std::min(KnobParameter::RadiusForHeight(cellHeight - titleHeight - mapButtonHeight - style.ItemSpacing.y),
								  KnobParameter::kDefaultRadius * scale);

	for (int row = 0; row < rows; ++row) {
		for (int column = 0; column < columns; ++column) {
			const int index = row * columns + column;
			if (index >= count)
				break;

			Parameter* macroParam = rack->GetMacroParameter(index);
			if (!macroParam)
				continue;

			const RackProcessor::Macro& macro = rack->GetMacro(index);
			const ImU32 color = macro.color != 0 ? macro.color : th.accent;
			const std::string title = macro.title.empty() ? macroParam->name : macro.title;

			ImGui::PushID(index);
			if (column > 0)
				ImGui::SameLine();
			ImGui::BeginGroup();

			// the title is drawn here rather than by the knob, so it can carry the
			// macro's own name, its color, and the menu that edits both
			const ImVec2 titlePos = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##MacroTitle", ImVec2(cellWidth, titleHeight));
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				OpenRename(RenameTarget::Macro, rack, index, title);
			if (ImGui::BeginPopupContextItem("MacroMenu")) {
				if (ImGui::MenuItem("Rename"))
					OpenRename(RenameTarget::Macro, rack, index, title);
				if (ImGui::BeginMenu("Color")) {
					ImU32 edited = macro.color;
					if (ColorMenu("MacroColor", edited)) {
						mDeferred.push_back([this, rack, index, edited]() {
							DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Macro color",
													[rack, index, edited]() { rack->GetMacroMutable(index).color = edited; });
						});
					}
					ImGui::EndMenu();
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Show mappings")) {
					mBrowserRack = rack;
					mOpenBrowserPopup = true;
				}
				if (ImGui::MenuItem("Unmap all", nullptr, false, !macro.mappings.empty())) {
					mDeferred.push_back([this, rack, index]() {
						DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Unmap macro",
												[rack, index]() { rack->GetMacroMutable(index).mappings.clear(); });
					});
				}
				ImGui::EndPopup();
			}

			const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
			ImGui::GetWindowDrawList()->AddText(ImVec2(titlePos.x + (cellWidth - titleSize.x) * 0.5f, titlePos.y),
												color, title.c_str());

			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::ColorConvertU32ToFloat4(color));
			if (radius > 0.0f) {
				static_cast<KnobParameter*>(macroParam)->DrawSized(radius, cellWidth, "");
			} else {
				macroParam->DrawCompact(cellWidth, "%.0f");
			}
			ImGui::PopStyleColor();

			if (mapMode) {
				ImGui::BeginDisabled(mMapCandidate == nullptr);
				if (ImGui::Button("Map", ImVec2(cellWidth - style.ItemSpacing.x, 0.0f))) {
					auto owner = rack->FindDeviceOwning(mMapCandidate);
					if (owner) {
						const std::string name = mMapCandidate->name;
						const float low = mMapCandidate->minValue;
						const float high = mMapCandidate->maxValue;
						mDeferred.push_back([this, rack, index, owner, name, low, high]() {
							DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Map macro",
													[&]() { rack->MapMacro(index, owner, name, low, high); });
						});
					}
				}
				ImGui::EndDisabled();
			}

			ImGui::EndGroup();
			ImGui::PopID();
		}
	}
}

void DeviceRackView::RenderChainList(const std::shared_ptr<RackProcessor>& rack, const ChainPath& path, int deviceIndex, float width, float height) {
	const Theme& th = Theme::Instance();
	const ImGuiStyle& style = ImGui::GetStyle();
	const float scale = mContext.state.mainScale;
	(void)height;

	auto& chains = rack->GetChains();
	const float buttonWidth = 18.0f * scale;
	const float volumeWidth = 46.0f * scale;
	const float nameWidth = std::max(width - buttonWidth * 2.0f - volumeWidth - style.ItemSpacing.x * 4.0f - style.ScrollbarSize, 40.0f);

	for (int i = 0; i < (int)chains.size(); ++i) {
		auto chain = chains[i];
		ImGui::PushID(i);

		const bool isSelected = (rack->GetSelectedChainIndex() == i);
		const ImU32 color = chain->GetColor() != 0 ? chain->GetColor() : (rack->GetColor() != 0 ? rack->GetColor() : th.accentMuted);

		if (ImGui::Selectable("##ChainRow", isSelected, ImGuiSelectableFlags_AllowOverlap, ImVec2(nameWidth, 0)))
			rack->SetSelectedChainIndex(i);

		// a device dropped on a chain row joins the end of that chain: the only way to
		// build a second branch without first making it the one on screen
		ChainPath chainPath;
		if (DeviceRackOps::DescendPath(path, deviceIndex, i, chainPath))
			AcceptDeviceDrop(chain, chainPath, (int)chain->GetProcessors().size());

		if (ImGui::BeginPopupContextItem("ChainMenu")) {
			if (ImGui::MenuItem("Rename"))
				OpenRename(RenameTarget::Chain, rack, i, chain->GetName());
			if (ImGui::BeginMenu("Color")) {
				ImU32 edited = chain->GetColor();
				if (ColorMenu("ChainColor", edited)) {
					mDeferred.push_back([this, rack, chain, edited]() {
						DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Chain color", [chain, edited]() { chain->SetColor(edited); });
					});
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			ImGui::TextDisabled("Volume");
			chain->GetVolumeParameter()->DrawCompact(120.0f * scale, nullptr);
			ImGui::TextDisabled("Pan");
			chain->GetPanParameter()->DrawCompact(120.0f * scale, "%.2f");
			ImGui::Separator();
			if (ImGui::MenuItem("Delete chain", nullptr, false, chains.size() > 1)) {
				mDeferred.push_back([this, rack, i]() {
					DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Delete chain", [rack, i]() { rack->RemoveChain(i); });
				});
			}
			ImGui::EndPopup();
		}

		const ImVec2 rowMin = ImGui::GetItemRectMin();
		const ImVec2 rowMax = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddRectFilled(rowMin, ImVec2(rowMin.x + 4.0f * scale, rowMax.y), color);
		const std::string label = chain->GetName().empty() ? ("Chain " + std::to_string(i + 1)) : chain->GetName();
		ImGui::GetWindowDrawList()->AddText(ImVec2(rowMin.x + 8.0f * scale, rowMin.y), th.text, label.c_str());

		ImGui::SameLine();
		if (ToggleButton("M", chain->GetMute(), ImVec2(buttonWidth, 0), th.danger)) {
			mDeferred.push_back([this, rack, chain]() {
				DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Mute chain", [chain]() { chain->SetMute(!chain->GetMute()); });
			});
		}
		ImGui::SameLine();
		if (ToggleButton("S", chain->GetSolo(), ImVec2(buttonWidth, 0), th.accent)) {
			mDeferred.push_back([this, rack, chain]() {
				DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Solo chain", [chain]() { chain->SetSolo(!chain->GetSolo()); });
			});
		}
		ImGui::SameLine();
		chain->GetVolumeParameter()->DrawCompact(volumeWidth, nullptr);

		ImGui::PopID();
	}

	if (ImGui::Button("+ Chain", ImVec2(width - style.ItemSpacing.x * 2.0f, 0))) {
		mDeferred.push_back([this, rack]() {
			DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Add chain", [rack]() {
				rack->AddChain("Chain " + std::to_string(rack->GetChains().size() + 1));
				rack->SetSelectedChainIndex((int)rack->GetChains().size() - 1);
			});
		});
	}
}

void DeviceRackView::RenderMappingBrowser(const std::shared_ptr<RackProcessor>& rack) {
	if (mBrowserRack.lock() != rack)
		return;

	ImGui::SetNextWindowSize(ImVec2(420.0f * mContext.state.mainScale, 0.0f), ImGuiCond_Appearing);
	if (!ImGui::BeginPopup("MacroMappings"))
		return;

	ImGui::TextDisabled("MACRO MAPPINGS");
	ImGui::Separator();

	bool anyMapping = false;
	for (int macroIndex = 0; macroIndex < RackProcessor::kMaxMacros; ++macroIndex) {
		const RackProcessor::Macro& macro = rack->GetMacro(macroIndex);
		if (macro.mappings.empty())
			continue;
		anyMapping = true;

		Parameter* macroParam = rack->GetMacroParameter(macroIndex);
		const std::string title = macro.title.empty() ? macroParam->name : macro.title;
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(macro.color != 0 ? macro.color : Theme::Instance().accent),
						   "%s", title.c_str());

		for (int mappingIndex = 0; mappingIndex < (int)macro.mappings.size(); ++mappingIndex) {
			const RackProcessor::MacroMapping& mapping = macro.mappings[mappingIndex];
			auto device = mapping.device.lock();

			ImGui::PushID(macroIndex * 100 + mappingIndex);
			ImGui::Text("%s", device ? device->GetName() : "(missing)");
			ImGui::SameLine(160.0f * mContext.state.mainScale);
			ImGui::Text("%s", mapping.paramName.c_str());

			// min and max are plain floats the audio thread reads, exactly like a
			// parameter value; the drag writes them live and reports one undo entry
			// when it is let go
			RackProcessor::Macro& editable = rack->GetMacroMutable(macroIndex);
			ImGui::SameLine(280.0f * mContext.state.mainScale);
			ImGui::SetNextItemWidth(56.0f * mContext.state.mainScale);
			ImGui::DragFloat("##Min", &editable.mappings[mappingIndex].minValue, 0.01f);
			if (ImGui::IsItemActivated()) {
				mRangeEditRack = rack;
				mRangeEditBefore = rack->CaptureState();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(56.0f * mContext.state.mainScale);
			ImGui::DragFloat("##Max", &editable.mappings[mappingIndex].maxValue, 0.01f);
			if (ImGui::IsItemActivated()) {
				mRangeEditRack = rack;
				mRangeEditBefore = rack->CaptureState();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) {
				mDeferred.push_back([this, rack, macroIndex, mappingIndex]() {
					DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Unmap macro",
											[rack, macroIndex, mappingIndex]() { rack->UnmapMacro(macroIndex, mappingIndex); });
				});
			}
			ImGui::PopID();
		}
	}

	if (!anyMapping)
		ImGui::TextDisabled("Nothing mapped yet. Turn on Map, click a device parameter,\nthen press Map under a macro.");

	// one history entry for the whole drag, pushed once the handle is let go
	if (auto editing = mRangeEditRack.lock()) {
		if (editing == rack && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			if (Project* project = mContext.GetProject()) {
				mContext.undoManager.Push(std::make_unique<RackStateAction>(project, rack, mRangeEditBefore,
																			rack->CaptureState(), "Macro range"));
			}
			mRangeEditRack.reset();
		}
	}

	ImGui::EndPopup();
}

// ================================================================
// MODULATOR
// ================================================================

void DeviceRackView::RenderModulatorBody(const std::shared_ptr<ModulatorProcessor>& modulator) {
	const Theme& th = Theme::Instance();

	// ---- map row ----
	// the same arm-then-hand-over gesture a rack macro takes, except that the parameter
	// may live anywhere in the project rather than having to be inside this device
	const bool mapMode = (mMapModeModulator.lock() == modulator);
	if (mapMode) {
		if (Parameter* clicked = Parameter::GetSelectedParameter())
			mMapCandidate = clicked;
	}
	if (ToggleButton("Map", mapMode, ImVec2(0, 0), th.accent))
		mMapModeModulator = mapMode ? std::weak_ptr<ModulatorProcessor>() : std::weak_ptr<ModulatorProcessor>(modulator);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Click a parameter anywhere - a rack macro, a plugin knob, a fader -\n"
						  "then press + to drive it from here");

	if (mapMode) {
		ImGui::SameLine();
		ImGui::BeginDisabled(mMapCandidate == nullptr);
		if (ImGui::Button("+")) {
			Parameter* candidate = mMapCandidate;
			mDeferred.push_back([this, modulator, candidate]() {
				Project* project = mContext.GetProject();
				DeviceRackOps::EditModulator(project, mContext.undoManager, modulator, "Modulation target",
											 [modulator, project, candidate]() { modulator->AddTarget(project, candidate); });
			});
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		if (mMapCandidate)
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(th.accent), "-> %s", mMapCandidate->name.c_str());
		else
			ImGui::TextDisabled("click a parameter");
	}

	ImGui::SameLine();
	if (ImGui::SmallButton("Targets"))
		mOpenTargetPopup = true;
	// the popup is opened here whoever asked for it: OpenPopup names an id relative to
	// the current stack (see the rack's mapping browser for the same dance)
	if (mOpenTargetPopup) {
		mBrowserModulator = modulator;
		mOpenTargetPopup = false;
		ImGui::OpenPopup("ModulationTargets");
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%d", (int)modulator->GetTargets().size());
	RenderTargetBrowser(modulator);

	// ---- generator ----
	modulator->RenderCustomUI(ImGui::GetContentRegionAvail());

	// a step drag or a randomize finished inside the device's own editor, where there was
	// no undo manager to report it to and no shared_ptr to keep the device alive with
	ModulatorProcessor::State before;
	ModulatorProcessor::State after;
	std::string name;
	if (modulator->TakePatternEdit(before, after, name)) {
		if (Project* project = mContext.GetProject()) {
			mContext.undoManager.Push(std::make_unique<ModulatorStateAction>(project, modulator, std::move(before),
																			 std::move(after), name));
		}
	}
}

void DeviceRackView::RenderTargetBrowser(const std::shared_ptr<ModulatorProcessor>& modulator) {
	if (mBrowserModulator.lock() != modulator)
		return;

	const float scale = mContext.state.mainScale;
	ImGui::SetNextWindowSize(ImVec2(470.0f * scale, 0.0f), ImGuiCond_Appearing);
	if (!ImGui::BeginPopup("ModulationTargets"))
		return;

	ImGui::TextDisabled("MODULATION TARGETS");
	ImGui::Separator();

	auto& targets = modulator->GetTargetsMutable();
	if (targets.empty()) {
		ImGui::TextDisabled("Nothing driven yet. Turn on Map, click a parameter,\n"
							"then press + to hand it to this modulator.");
	}

	for (int i = 0; i < (int)targets.size(); ++i) {
		ModulatorProcessor::Target& target = targets[i];
		ImGui::PushID(i);

		bool enabled = target.enabled;
		if (ImGui::Checkbox("##On", &enabled)) {
			const bool wanted = enabled;
			mDeferred.push_back([this, modulator, i, wanted]() {
				DeviceRackOps::EditModulator(mContext.GetProject(), mContext.undoManager, modulator, "Modulation target",
											 [modulator, i, wanted]() {
												 auto& list = modulator->GetTargetsMutable();
												 if (i < (int)list.size())
													 list[i].enabled = wanted;
											 });
			});
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Stop driving this parameter without forgetting the range");

		ImGui::SameLine();
		ImGui::Text("%s", target.deviceName.empty() ? "(missing)" : target.deviceName.c_str());
		if (ImGui::IsItemHovered() && !target.trackName.empty())
			ImGui::SetTooltip("on %s", target.trackName.c_str());

		ImGui::SameLine(190.0f * scale);
		ImGui::Text("%s", target.paramName.c_str());

		// min and max are plain floats the audio thread reads, exactly like a parameter
		// value; the drag writes them live and reports one undo entry when it is let go
		ImGui::SameLine(320.0f * scale);
		ImGui::SetNextItemWidth(56.0f * scale);
		ImGui::DragFloat("##Min", &target.minValue, 0.01f);
		if (ImGui::IsItemActivated()) {
			mRangeEditModulator = modulator;
			mRangeEditModulatorBefore = modulator->CaptureState();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(56.0f * scale);
		ImGui::DragFloat("##Max", &target.maxValue, 0.01f);
		if (ImGui::IsItemActivated()) {
			mRangeEditModulator = modulator;
			mRangeEditModulatorBefore = modulator->CaptureState();
		}

		ImGui::SameLine();
		if (ImGui::SmallButton("x")) {
			mDeferred.push_back([this, modulator, i]() {
				DeviceRackOps::EditModulator(mContext.GetProject(), mContext.undoManager, modulator, "Drop target",
											 [modulator, i]() { modulator->RemoveTarget(i); });
			});
		}
		ImGui::PopID();
	}

	// one history entry for the whole drag, pushed once the handle is let go
	if (auto editing = mRangeEditModulator.lock()) {
		if (editing == modulator && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			if (Project* project = mContext.GetProject()) {
				mContext.undoManager.Push(std::make_unique<ModulatorStateAction>(project, modulator, mRangeEditModulatorBefore,
																				 modulator->CaptureState(), "Modulation range"));
			}
			mRangeEditModulator.reset();
		}
	}

	ImGui::EndPopup();
}

// ================================================================
// GESTURES
// ================================================================

bool DeviceRackView::AcceptDeviceDrop(const std::shared_ptr<ProcessorHost>& host, const ChainPath& path, int index) {
	if (!ImGui::BeginDragDropTarget())
		return false;

	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PROCESSOR_MOVE")) {
		const DevicePath source = *(const DevicePath*)payload->Data;
		mDeferred.push_back([this, source, host, index]() {
			auto location = DeviceRackOps::ResolveDevice(mContext.GetProject(), source);
			if (location.IsValid())
				DeviceRackOps::MoveDevice(mContext.GetProject(), mContext.undoManager, location.host, location.index, host, index);
		});
	}
	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST_PLUGIN")) {
		const std::string pluginPath = (const char*)payload->Data;
		auto vST = std::make_shared<VSTProcessor>(pluginPath);
		if (vST->Load())
			mDeferred.push_back([this, host, index, vST]() { DeviceRackOps::InsertDevice(mContext.GetProject(), mContext.undoManager, host, index, vST, "Add device"); });
	}
	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST3_PLUGIN")) {
		const std::string data = (const char*)payload->Data;
		const size_t pipe = data.find('|');
		if (pipe != std::string::npos) {
			auto vST3 = std::make_shared<VST3Processor>(data.substr(0, pipe), data.substr(pipe + 1));
			if (vST3->Load())
				mDeferred.push_back([this, host, index, vST3]() { DeviceRackOps::InsertDevice(mContext.GetProject(), mContext.undoManager, host, index, vST3, "Add device"); });
		}
	}
	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INTERNAL_PLUGIN")) {
		auto device = ProcessorFactory::Instance().Create((const char*)payload->Data);
		if (device) {
			// a rack dropped from the library arrives empty, and an empty rack is a
			// pass-through: give it the one chain the user is about to fill
			if (auto rack = AsRack(device))
				rack->AddChain("Chain");
			mDeferred.push_back([this, host, index, device]() { DeviceRackOps::InsertDevice(mContext.GetProject(), mContext.undoManager, host, index, device, "Add device"); });
		}
	}

	ImGui::EndDragDropTarget();
	return true;
}

void DeviceRackView::DeleteDevices(const std::vector<std::shared_ptr<AudioProcessor>>& devices) {
	DeviceRackOps::RemoveDevices(mContext.GetProject(), mContext.undoManager, mTrack, devices);
	mContext.state.ClearDeviceSelection();
}

void DeviceRackView::GroupDevices(const std::vector<std::shared_ptr<AudioProcessor>>& devices) {
	auto rack = DeviceRackOps::GroupDevices(mContext.GetProject(), mContext.undoManager, mTrack, devices);
	if (rack)
		mContext.state.SelectDevice(rack);
}

void DeviceRackView::UngroupRack(const std::shared_ptr<RackProcessor>& rack) {
	auto released = DeviceRackOps::UngroupRack(mContext.GetProject(), mContext.undoManager, mTrack, rack);
	mContext.state.SetDeviceSelection(std::move(released));
}

void DeviceRackView::ClickDevice(const std::shared_ptr<AudioProcessor>& device) {
	const ImGuiIO& io = ImGui::GetIO();
	if (io.KeyShift)
		DeviceRackOps::SelectRangeTo(mContext.state, mTrack, device);
	else if (io.KeyCtrl)
		mContext.state.ToggleDeviceSelection(device);
	else
		mContext.state.SelectDevice(device);
}

std::vector<std::shared_ptr<AudioProcessor>> DeviceRackView::CommandTargets(const std::shared_ptr<AudioProcessor>& device) const {
	if (mContext.state.IsDeviceSelected(device))
		return mContext.state.selectedDevices;
	return {device};
}

void DeviceRackView::RenderDeviceContextMenu(const std::shared_ptr<AudioProcessor>& device) {
	if (!ImGui::BeginPopupContextItem("DeviceMenu"))
		return;

	auto targets = CommandTargets(device);
	auto rack = AsRack(device);
	const int count = (int)targets.size();

	if (ImGui::MenuItem(count > 1 ? "Copy Devices" : "Copy")) {
		std::vector<std::shared_ptr<AudioProcessor>> copies;
		for (const auto& target : targets) {
			if (auto clone = ProcessorIO::CloneProcessor(target))
				copies.push_back(clone);
		}
		mContext.state.processorClipboard = std::move(copies);
	}
	if (ImGui::MenuItem("Paste", nullptr, false, !mContext.state.processorClipboard.empty())) {
		auto clipboard = mContext.state.processorClipboard;
		mDeferred.push_back([this, device, clipboard]() {
			auto location = DeviceRackOps::Locate(mTrack, device);
			if (!location.IsValid())
				return;
			for (int i = 0; i < (int)clipboard.size(); ++i) {
				if (auto clone = ProcessorIO::CloneProcessor(clipboard[i]))
					DeviceRackOps::InsertDevice(mContext.GetProject(), mContext.undoManager, location.host, location.index + 1 + i, clone, "Paste device");
			}
		});
	}
	if (ImGui::MenuItem(count > 1 ? "Duplicate Devices" : "Duplicate")) {
		mDeferred.push_back([this, targets]() {
			for (const auto& target : targets)
				DeviceRackOps::DuplicateDevice(mContext.GetProject(), mContext.undoManager, mTrack, target);
		});
	}

	ImGui::Separator();
	const bool anyActive = std::any_of(targets.begin(), targets.end(),
									   [](const std::shared_ptr<AudioProcessor>& target) { return !target->IsBypassed(); });
	if (ImGui::MenuItem(anyActive ? "Deactivate" : "Activate")) {
		mDeferred.push_back([this, targets]() { DeviceRackOps::ToggleDevicesBypassed(mContext.GetProject(), mContext.undoManager, targets); });
	}
	if (ImGui::MenuItem("Group", "Ctrl+G")) {
		mDeferred.push_back([this, targets]() { GroupDevices(targets); });
	}
	if (rack) {
		const bool canUngroup = DeviceRackOps::CanUngroup(rack);
		if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G", false, canUngroup))
			mDeferred.push_back([this, rack]() { UngroupRack(rack); });
		if (!canUngroup && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("A rack with parallel chains has no serial arrangement to flatten into");
		if (ImGui::MenuItem("Rename"))
			OpenRename(RenameTarget::Rack, rack, -1, rack->GetName());
		if (ImGui::BeginMenu("Color")) {
			ImU32 edited = rack->GetColor();
			if (ColorMenu("RackColor", edited)) {
				mDeferred.push_back([this, rack, edited]() {
					DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Rack color", [rack, edited]() { rack->SetColor(edited); });
				});
			}
			ImGui::EndMenu();
		}
	}

	// per-plugin high-DPI override for the editor window
	if (device->HasEditor()) {
		ImGui::Separator();
		if (ImGui::BeginMenu("Editor Scaling")) {
			const bool globalNative = AppConfig::Instance().pluginEditorsNative;
			const EditorScalingMode mode = device->GetEditorScalingMode();
			EditorScalingMode newMode = mode;

			const std::string defaultLabel = std::string("Use Global Default (") + (globalNative ? "Native" : "Scaled") + ")";
			if (ImGui::MenuItem(defaultLabel.c_str(), nullptr, mode == EditorScalingMode::Default))
				newMode = EditorScalingMode::Default;
			if (ImGui::MenuItem("Native (crisp)", nullptr, mode == EditorScalingMode::Native))
				newMode = EditorScalingMode::Native;
			if (ImGui::MenuItem("Scaled (match DAW)", nullptr, mode == EditorScalingMode::Scaled))
				newMode = EditorScalingMode::Scaled;

			if (newMode != mode) {
				device->SetEditorScalingMode(newMode);
				// re-open the window so the new DPI mode takes effect immediately
				if (device->IsEditorOpen()) {
					device->CloseEditor();
					device->OpenEditor(mContext.nativeWindowHandle);
				}
			}
			ImGui::EndMenu();
		}
	}

	ImGui::Separator();
	if (ImGui::MenuItem(count > 1 ? "Delete Devices" : "Delete", "Del")) {
		mDeferred.push_back([this, targets]() { DeleteDevices(targets); });
	}
	ImGui::EndPopup();
}

void DeviceRackView::HandleShortcuts() {
	const ImGuiIO& io = ImGui::GetIO();
	const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	auto selection = mContext.state.selectedDevices;

	// Ctrl+G means "group what I am looking at". the editor's global handler groups
	// tracks unless the rack says it has something of its own to group
	mContext.state.deviceRackOwnsGroupShortcut = focused && !selection.empty();

	if (!focused || io.WantTextInput)
		return;

	if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		mContext.state.ClearDeviceSelection();

	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A) && mTrack) {
		auto host = std::static_pointer_cast<ProcessorHost>(mTrack);
		if (auto focus = mContext.state.selectedDevice) {
			auto location = DeviceRackOps::Locate(mTrack, focus);
			if (location.IsValid())
				host = location.host;
		}
		mContext.state.SetDeviceSelection(host->GetProcessors());
	}

	if (selection.empty())
		return;

	if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
		mDeferred.push_back([this, selection]() { DeleteDevices(selection); });

	// 0 activates and deactivates, the same key the reference product uses
	if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0))
		mDeferred.push_back([this, selection]() { DeviceRackOps::ToggleDevicesBypassed(mContext.GetProject(), mContext.undoManager, selection); });

	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G)) {
		if (io.KeyShift) {
			if (auto rack = AsRack(mContext.state.selectedDevice))
				mDeferred.push_back([this, rack]() { UngroupRack(rack); });
		} else {
			mDeferred.push_back([this, selection]() { GroupDevices(selection); });
		}
	}

	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
		std::vector<std::shared_ptr<AudioProcessor>> copies;
		for (const auto& device : selection) {
			if (auto clone = ProcessorIO::CloneProcessor(device))
				copies.push_back(clone);
		}
		mContext.state.processorClipboard = std::move(copies);
	}
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
		mDeferred.push_back([this, selection]() {
			for (const auto& device : selection)
				DeviceRackOps::DuplicateDevice(mContext.GetProject(), mContext.undoManager, mTrack, device);
		});
	}
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && !mContext.state.processorClipboard.empty()) {
		auto clipboard = mContext.state.processorClipboard;
		auto focus = mContext.state.selectedDevice;
		mDeferred.push_back([this, clipboard, focus]() {
			auto location = DeviceRackOps::Locate(mTrack, focus);
			if (!location.IsValid())
				return;
			for (int i = 0; i < (int)clipboard.size(); ++i) {
				if (auto clone = ProcessorIO::CloneProcessor(clipboard[i]))
					DeviceRackOps::InsertDevice(mContext.GetProject(), mContext.undoManager, location.host, location.index + 1 + i, clone, "Paste device");
			}
		});
	}
}

// ================================================================
// RENAME
// ================================================================

void DeviceRackView::OpenRename(RenameTarget target, const std::shared_ptr<RackProcessor>& rack, int index, const std::string& current) {
	mRenameTarget = target;
	mRenameRack = rack;
	mRenameIndex = index;
	mOpenRenamePopup = true;
	strncpy(mRenameBuffer, current.c_str(), sizeof(mRenameBuffer) - 1);
	mRenameBuffer[sizeof(mRenameBuffer) - 1] = 0;
}

void DeviceRackView::RenderRenamePopup() {
	if (mOpenRenamePopup) {
		ImGui::OpenPopup("Rename##DeviceRack");
		mOpenRenamePopup = false;
	}

	if (!ImGui::BeginPopup("Rename##DeviceRack"))
		return;

	auto rack = mRenameRack.lock();
	if (!rack || mRenameTarget == RenameTarget::None) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}

	if (ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	const bool committed = ImGui::InputText("##RenameText", mRenameBuffer, sizeof(mRenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
	if (committed) {
		const std::string name = mRenameBuffer;
		const RenameTarget target = mRenameTarget;
		const int index = mRenameIndex;
		mDeferred.push_back([this, rack, target, index, name]() {
			DeviceRackOps::EditRack(mContext.GetProject(), mContext.undoManager, rack, "Rename", [rack, target, index, name]() {
				switch (target) {
				case RenameTarget::Rack:
					rack->SetName(name);
					break;
				case RenameTarget::Macro:
					rack->GetMacroMutable(index).title = name;
					break;
				case RenameTarget::Chain:
					if (index >= 0 && index < (int)rack->GetChains().size())
						rack->GetChains()[index]->SetName(name);
					break;
				case RenameTarget::None:
					break;
				}
			});
		});
		mRenameTarget = RenameTarget::None;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}
