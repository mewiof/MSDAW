#include "PrecompHeader.h"
#include "DeviceRackView.h"
#include "AppConfig.h"
#include "Project.h"
#include "Track.h"
#include "ProcessorFactory.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"
#include <filesystem>
#include <algorithm>
#include <mutex>

// definitions

struct DeviceMovePayload {
	int trackIndex;
	int deviceIndex;
};

// helper to clone a processor (factory style)
static std::shared_ptr<AudioProcessor> CloneProcessor(std::shared_ptr<AudioProcessor> src) {
	if (!src)
		return nullptr;

	std::shared_ptr<AudioProcessor> dst = nullptr;

	// 1. instantiate correct type
	if (auto vST = std::dynamic_pointer_cast<VSTProcessor>(src)) {
		dst = std::make_shared<VSTProcessor>(vST->GetPath());
		if (!std::dynamic_pointer_cast<VSTProcessor>(dst)->Load())
			return nullptr;
	} else if (auto vST3 = std::dynamic_pointer_cast<VST3Processor>(src)) {
		dst = std::make_shared<VST3Processor>(vST3->GetPath(), vST3->GetClassID());
		if (!std::dynamic_pointer_cast<VST3Processor>(dst)->Load())
			return nullptr;
	} else
		dst = ProcessorFactory::Instance().Create(src->GetProcessorId());

	if (!dst)
		return nullptr;

	// 2. copy parameters
	const auto& srcParams = src->GetParameters();
	const auto& dstParams = dst->GetParameters();

	// assuming param order is identical for same class
	for (size_t i = 0; i < srcParams.size() && i < dstParams.size(); ++i) {
		dstParams[i]->value = srcParams[i]->value;
	}

	// 3. copy state
	dst->SetBypassed(src->IsBypassed());

	return dst;
}

// render implementation

void DeviceRackView::Render(const ImVec2& pos, float width, float height) {
	ImVec2 defaultPadding = ImGui::GetStyle().WindowPadding;
	// prevent double-padding issues with full-size children
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::Begin("Device Rack", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	Project* project = mContext.GetProject();
	int trackIdx = mContext.state.selectedTrackIndex;

	std::shared_ptr<Track> selectedTrack = nullptr;

	if (project) {
		if (trackIdx == -1) {
			selectedTrack = project->GetMasterTrack();
		} else if (trackIdx >= 0 && trackIdx < (int)project->GetTracks().size()) {
			selectedTrack = project->GetTracks()[trackIdx];
		}
	}

	struct PendingAction {
		enum Type { MoveReq,
					RemoveReq,
					DuplicateReq,
					CopyReq,
					PasteReq } type;
		int srcIdx = -1;
		int dstIdx = -1;
	};
	PendingAction action = {PendingAction::MoveReq, -1, -1};
	bool hasAction = false;

	if (selectedTrack) {
		// use horizontal scrolling for the devices area
		ImGui::BeginChild("DevicesArea", ImVec2(width, height), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		auto& processors = selectedTrack->GetProcessors();

		// iterate processors with drag targets in between
		for (int i = 0; i <= (int)processors.size(); ++i) {
			// drop target (insert between)
			ImGui::PushID(i * 1000 + 999);

			float dropWidth = 10.0f * mContext.state.mainScale;
			// at the very end, extend drop zone to fill remaining space
			if (i == (int)processors.size()) {
				float availWidth = ImGui::GetContentRegionAvail().x;
				if (availWidth > dropWidth)
					dropWidth = availWidth;
			}

			ImGui::InvisibleButton("##DropZone", ImVec2(dropWidth, height));

			// reordering acceptance
			if (ImGui::BeginDragDropTarget()) {
				// 1. move within/between tracks
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PROCESSOR_MOVE")) {
					DeviceMovePayload* moveData = (DeviceMovePayload*)payload->Data;
					if (moveData->trackIndex == trackIdx) {
						action.type = PendingAction::MoveReq;
						action.srcIdx = moveData->deviceIndex;
						action.dstIdx = i;
						hasAction = true;
					} else {
						// logic to move from other track to this one
						std::shared_ptr<Track> srcTrackPtr = nullptr;
						if (moveData->trackIndex == -1)
							srcTrackPtr = project->GetMasterTrack();
						else if (moveData->trackIndex >= 0 && moveData->trackIndex < (int)project->GetTracks().size())
							srcTrackPtr = project->GetTracks()[moveData->trackIndex];

						if (srcTrackPtr && moveData->deviceIndex < (int)srcTrackPtr->GetProcessors().size()) {
							std::lock_guard<std::mutex> lock(project->GetMutex());
							auto proc = srcTrackPtr->GetProcessors()[moveData->deviceIndex];
							srcTrackPtr->RemoveProcessor(moveData->deviceIndex);
							selectedTrack->InsertProcessor(i, proc);
							ImGui::EndDragDropTarget();
							ImGui::PopID();
							break;
						}
					}
				}
				// 2. new VST
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST_PLUGIN")) {
					std::string path = (const char*)payload->Data;
					auto vST = std::make_shared<VSTProcessor>(path);
					if (vST->Load()) {
						std::lock_guard<std::mutex> lock(project->GetMutex());
						selectedTrack->InsertProcessor(i, vST);
						if (project->GetTransport().GetSampleRate() > 0)
							vST->PrepareToPlay(project->GetTransport().GetSampleRate());
					}
				}
				// 3. new VST3
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VST3_PLUGIN")) {
					std::string data = (const char*)payload->Data;
					size_t pipe = data.find('|');
					if (pipe != std::string::npos) {
						std::string path = data.substr(0, pipe);
						std::string classID = data.substr(pipe + 1);
						auto vST = std::make_shared<VST3Processor>(path, classID);
						if (vST->Load()) {
							std::lock_guard<std::mutex> lock(project->GetMutex());
							selectedTrack->InsertProcessor(i, vST);
							if (project->GetTransport().GetSampleRate() > 0)
								vST->PrepareToPlay(project->GetTransport().GetSampleRate());
						}
					}
				}
				// 4. new internal
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INTERNAL_PLUGIN")) {
					std::string type = (const char*)payload->Data;
					std::shared_ptr<AudioProcessor> proc = ProcessorFactory::Instance().Create(type);

					if (proc) {
						std::lock_guard<std::mutex> lock(project->GetMutex());
						selectedTrack->InsertProcessor(i, proc);
						if (project->GetTransport().GetSampleRate() > 0)
							proc->PrepareToPlay(project->GetTransport().GetSampleRate());
					}
				}

				ImGui::EndDragDropTarget();
			}
			ImGui::PopID();

			// stop loop if at end
			if (i == (int)processors.size())
				break;

			// draw device
			auto& proc = processors[i];
			bool isInstrument = proc->IsInstrument();
			ImGui::SameLine();

			// visual container setup
			ImGui::PushID(proc.get());
			// color based on active/bypassed
			ImU32 bgCol = isInstrument ? IM_COL32(45, 45, 50, 255) : IM_COL32(35, 35, 38, 255);
			if (proc->IsBypassed())
				bgCol = IM_COL32(20, 20, 22, 255);

			ImGui::PushStyleColor(ImGuiCol_ChildBg, bgCol);

			// increased device width for side-by-side controls
			float deviceWidth = 280.0f * mContext.state.mainScale;
			std::string pId = proc->GetProcessorId();
			// special case wider devices
			if (pId == "EqEight")
				deviceWidth = 350.0f * mContext.state.mainScale;
			if (pId == "DelayReverb")
				deviceWidth = 320.0f * mContext.state.mainScale;

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, defaultPadding);
			ImGui::BeginChild("DeviceBody", ImVec2(deviceWidth, height), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PopStyleVar();

			// header: enable toggle & name
			ImGui::BeginGroup();

			// enable/disable toggle
			bool active = !proc->IsBypassed();
			ImU32 ledCol = active ? IM_COL32(255, 200, 0, 255) : IM_COL32(60, 60, 60, 255);
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##BypassToggle", ImVec2(12, 12));
			if (ImGui::IsItemClicked()) {
				proc->SetBypassed(active);
			}
			ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 6, p.y + 6), 5.0f, ledCol);

			ImGui::SameLine();
			ImGui::Text("%s", proc->GetName());

			// move "open editor" to header to ensure visibility regardless of state
			if (proc->HasEditor()) {
				ImGui::SameLine();
				if (ImGui::SmallButton("Edit")) {
					proc->OpenEditor(mContext.nativeWindowHandle);
				}
			}

			ImGui::EndGroup();

			// context menu for device
			if (ImGui::BeginPopupContextItem("DeviceCtx")) {
				if (ImGui::MenuItem("Copy")) {
					mContext.state.processorClipboard = CloneProcessor(proc);
				}
				if (ImGui::MenuItem("Paste", nullptr, false, mContext.state.processorClipboard != nullptr)) {
					action.type = PendingAction::PasteReq;
					action.dstIdx = i + 1;
					hasAction = true;
				}
				if (ImGui::MenuItem("Duplicate")) {
					action.type = PendingAction::DuplicateReq;
					action.srcIdx = i;
					hasAction = true;
				}

				// per-plugin high-DPI override for the editor window
				if (proc->HasEditor()) {
					ImGui::Separator();
					if (ImGui::BeginMenu("Editor Scaling")) {
						bool globalNative = AppConfig::Instance().pluginEditorsNative;
						EditorScalingMode mode = proc->GetEditorScalingMode();
						EditorScalingMode newMode = mode;

						std::string defaultLabel = std::string("Use Global Default (") + (globalNative ? "Native" : "Scaled") + ")";
						if (ImGui::MenuItem(defaultLabel.c_str(), nullptr, mode == EditorScalingMode::Default))
							newMode = EditorScalingMode::Default;
						if (ImGui::MenuItem("Native (crisp)", nullptr, mode == EditorScalingMode::Native))
							newMode = EditorScalingMode::Native;
						if (ImGui::MenuItem("Scaled (match DAW)", nullptr, mode == EditorScalingMode::Scaled))
							newMode = EditorScalingMode::Scaled;

						if (newMode != mode) {
							proc->SetEditorScalingMode(newMode);
							// re-open the window so the new DPI mode takes effect immediately
							if (proc->IsEditorOpen()) {
								proc->CloseEditor();
								proc->OpenEditor(mContext.nativeWindowHandle);
							}
						}
						ImGui::EndMenu();
					}
				}

				ImGui::Separator();
				if (ImGui::MenuItem("Delete")) {
					action.type = PendingAction::RemoveReq;
					action.srcIdx = i;
					hasAction = true;
				}
				ImGui::EndPopup();
			}

			// drag source
			ImGui::SetCursorPos(ImVec2(0, 0));
			ImGui::InvisibleButton("##DragHandle", ImVec2(deviceWidth, 25));
			if (ImGui::BeginDragDropSource()) {
				DeviceMovePayload payload;
				payload.trackIndex = trackIdx;
				payload.deviceIndex = i;
				ImGui::SetDragDropPayload("PROCESSOR_MOVE", &payload, sizeof(DeviceMovePayload));
				ImGui::Text("%s", proc->GetName());
				ImGui::EndDragDropSource();
			}
			if (ImGui::BeginPopupContextItem("DeviceCtx_Handle")) {
				if (ImGui::MenuItem("Delete")) {
					action.type = PendingAction::RemoveReq;
					action.srcIdx = i;
					hasAction = true;
				}
				ImGui::EndPopup();
			}

			ImGui::Separator();

			// parameters/ui
			ImVec2 avail = ImGui::GetContentRegionAvail();
			if (!proc->IsBypassed()) {
				if (!proc->RenderCustomUI(avail)) {
					ImGui::BeginChild("ParamsScroll", ImVec2(0, 0));
					for (auto& param : proc->GetParameters())
						param->Draw();
					ImGui::EndChild();
				}
			} else {
				ImGui::TextDisabled("Device Bypassed");
			}

			ImGui::EndChild();
			ImGui::PopStyleColor();
			ImGui::PopID();

			ImGui::SameLine();
		}

		// handle actions
		if (hasAction) {
			// lock during graph modification
			// this prevents the audio thread from iterating the processors list
			// while we are removing an item -> unloading its DLL
			std::lock_guard<std::mutex> lock(project->GetMutex());

			if (action.type == PendingAction::MoveReq) {
				selectedTrack->MoveProcessor(action.srcIdx, action.dstIdx);
			} else if (action.type == PendingAction::RemoveReq) {
				selectedTrack->RemoveProcessor(action.srcIdx);
			} else if (action.type == PendingAction::DuplicateReq) {
				auto clone = CloneProcessor(processors[action.srcIdx]);
				if (clone) {
					if (project->GetTransport().GetSampleRate() > 0)
						clone->PrepareToPlay(project->GetTransport().GetSampleRate());
					selectedTrack->InsertProcessor(action.srcIdx + 1, clone);
				}
			} else if (action.type == PendingAction::PasteReq) {
				auto clone = CloneProcessor(mContext.state.processorClipboard);
				if (clone) {
					if (project->GetTransport().GetSampleRate() > 0)
						clone->PrepareToPlay(project->GetTransport().GetSampleRate());
					selectedTrack->InsertProcessor(action.dstIdx, clone);
				}
			}
		}

		if (processors.empty()) {
			ImGui::SetCursorPos(ImVec2(20, height / 2));
			ImGui::TextDisabled("Drag Instruments or Effects here from the Library");
		}

		ImGui::EndChild();
	} else {
		ImGui::TextDisabled("No Track Selected. Select a track to view devices.");
	}
	ImGui::End();
	ImGui::PopStyleVar();
}
