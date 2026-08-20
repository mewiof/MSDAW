#include "PrecompHeader.h"
#include "LibraryView.h"
#include "Theme.h"
#include <algorithm>

void LibraryView::Render(const ImVec2& pos, float width, float height) {
	const Theme& th = Theme::Instance();
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::Begin("Library", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

	// internal effects
	ImGui::PushStyleColor(ImGuiCol_Text, th.textMuted);
	ImGui::Text("INTERNAL");
	ImGui::Separator();
	ImGui::PopStyleColor();

	// bit crusher
	ImGui::PushID("BitCrusher");
	if (ImGui::Selectable("Bit Crusher")) {
	}
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("INTERNAL_PLUGIN", "BitCrusher", strlen("BitCrusher") + 1);
		ImGui::Text("Bit Crusher");
		ImGui::TextDisabled("Effect");
		ImGui::EndDragDropSource();
	}
	ImGui::PopID();

	// auto sidechain
	ImGui::PushID("AutoSidechain");
	if (ImGui::Selectable("Auto Sidechain")) {
	}
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("INTERNAL_PLUGIN", "AutoSidechain", strlen("AutoSidechain") + 1);
		ImGui::Text("Auto Sidechain");
		ImGui::TextDisabled("Effect");
		ImGui::EndDragDropSource();
	}
	ImGui::PopID();

	ImGui::Dummy(ImVec2(0, 10));

	// VST plugins
	ImGui::PushStyleColor(ImGuiCol_Text, th.textMuted);
	ImGui::Text("PLUGINS");
	ImGui::Separator();
	ImGui::PopStyleColor();

	const auto& plugins = mContext.pluginManager.GetKnownPlugins();

	if (plugins.empty()) {
		ImGui::TextDisabled("No plugins found.");
		ImGui::TextDisabled("Check Settings.");
	}

	// filter / list
	static ImGuiTextFilter filter;
	// drive the filter through a hinted input rather than ImGuiTextFilter::Draw: its label
	// renders to the right of the box and spilled a clipped half-letter past the panel edge
	// a placeholder hint keeps the box clean and inset by the window padding on both sides
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	if (ImGui::InputTextWithHint("##libSearch", "Search", filter.InputBuf, IM_ARRAYSIZE(filter.InputBuf)))
		filter.Build();

	ImGui::BeginChild("LibList");
	for (const auto& plugin : plugins) {
		if (!filter.PassFilter(plugin.name.c_str()))
			continue;

		ImGui::PushID(plugin.path.c_str());

		// color code instruments vs effects
		if (plugin.isSynth)
			ImGui::PushStyleColor(ImGuiCol_Text, th.graphCurveCool); // instruments read cool
		else
			ImGui::PushStyleColor(ImGuiCol_Text, th.text);

		std::string label = plugin.name + " (" + plugin.format + ")";
		if (ImGui::Selectable(label.c_str())) {
		}

		ImGui::PopStyleColor();

		// drag source for VST
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
			if (plugin.format == "VST3") {
				std::string payloadStr = plugin.path + "|" + plugin.classID;
				ImGui::SetDragDropPayload("VST3_PLUGIN", payloadStr.c_str(), payloadStr.size() + 1);
			} else {
				ImGui::SetDragDropPayload("VST_PLUGIN", plugin.path.c_str(), plugin.path.size() + 1);
			}

			// preview
			ImGui::Text("%s", plugin.name.c_str());
			ImGui::TextDisabled("%s | %s", plugin.format.c_str(), plugin.isSynth ? "Instrument" : "Effect");

			ImGui::EndDragDropSource();
		}
		ImGui::PopID();
	}
	ImGui::EndChild();

	ImGui::End();
}
