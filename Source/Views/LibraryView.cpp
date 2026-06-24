#include "PrecompHeader.h"
#include "LibraryView.h"
#include <algorithm>

void LibraryView::Render(const ImVec2& pos, float width, float height) {
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::Begin("Library", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

	// internal instruments
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 180, 180, 255));
	ImGui::Text("INTERNAL");
	ImGui::Separator();
	ImGui::PopStyleColor();

	// simple synth
	ImGui::PushID("SimpleSynth");
	if (ImGui::Selectable("Simple Synth")) {
	}
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("INTERNAL_PLUGIN", "SimpleSynth", strlen("SimpleSynth") + 1);
		ImGui::Text("Simple Synth");
		ImGui::TextDisabled("Instrument");
		ImGui::EndDragDropSource();
	}
	ImGui::PopID();

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

	// eq eight
	ImGui::PushID("EqEight");
	if (ImGui::Selectable("EQ Eight")) {
	}
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("INTERNAL_PLUGIN", "EqEight", strlen("EqEight") + 1);
		ImGui::Text("EQ Eight");
		ImGui::TextDisabled("Effect");
		ImGui::EndDragDropSource();
	}
	ImGui::PopID();

	// ott
	ImGui::PushID("OTT");
	if (ImGui::Selectable("OTT")) {
	}
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("INTERNAL_PLUGIN", "OTT", strlen("OTT") + 1);
		ImGui::Text("OTT");
		ImGui::TextDisabled("Effect");
		ImGui::EndDragDropSource();
	}
	ImGui::PopID();

	// delay reverb
	ImGui::PushID("DelayReverb");
	if (ImGui::Selectable("DelayReverb")) {
	}
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("INTERNAL_PLUGIN", "DelayReverb", strlen("DelayReverb") + 1);
		ImGui::Text("DelayReverb");
		ImGui::TextDisabled("Effect");
		ImGui::EndDragDropSource();
	}
	ImGui::PopID();

	ImGui::Dummy(ImVec2(0, 10));

	// VST plugins
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 180, 180, 255));
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
	filter.Draw("Search##lib", width - 10);

	ImGui::BeginChild("LibList");
	for (const auto& plugin : plugins) {
		if (!filter.PassFilter(plugin.name.c_str()))
			continue;

		ImGui::PushID(plugin.path.c_str());

		// color code instruments vs effects
		if (plugin.isSynth)
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 220, 255, 255));
		else
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 220, 200, 255));

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
