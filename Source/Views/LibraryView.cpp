#include "PrecompHeader.h"
#include "LibraryView.h"
#include "AppConfig.h"
#include "Theme.h"
#include <algorithm>
#include <cmath>

void LibraryView::Render(const ImVec2& pos, float width, float height) {
	const Theme& th = Theme::Instance();
	AppConfig& config = AppConfig::Instance();

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));

	// ---- folded away: the panel is a rail that brings it back ----
	// Editor has already sized the window down to the rail; the whole strip is the
	// hit target, since a column this narrow has no room for a labelled button
	if (config.libraryCollapsed) {
		// the default window padding is wider than the rail itself: center the arrow in
		// what room there is instead of letting it hang off the edge
		const float arrowSize = ImGui::GetFrameHeight();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
							ImVec2(std::max((width - arrowSize) * 0.5f, 0.0f), ImGui::GetStyle().WindowPadding.y));
		ImGui::Begin("Library", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
		ImGui::PopStyleVar();

		bool expand = ImGui::ArrowButton("##ExpandLibrary", ImGuiDir_Right);
		bool hovered = ImGui::IsItemHovered();

		float restHeight = ImGui::GetContentRegionAvail().y;
		if (restHeight > 1.0f) {
			expand = ImGui::InvisibleButton("##ExpandLibraryRail", ImVec2(arrowSize, restHeight)) || expand;
			hovered = hovered || ImGui::IsItemHovered();
		}
		if (hovered)
			ImGui::SetTooltip("Show the library");
		if (expand) {
			config.libraryCollapsed = false;
			config.Save();
		}

		ImGui::End();
		return;
	}

	ImGui::Begin("Library", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

	// internal effects. the collapse handle rides the first section header rather than
	// taking a row of its own - the library is already the narrowest column on screen
	if (ImGui::ArrowButton("##CollapseLibrary", ImGuiDir_Left)) {
		config.libraryCollapsed = true;
		config.Save();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Hide the library");
	ImGui::SameLine();

	ImGui::PushStyleColor(ImGuiCol_Text, th.textMuted);
	ImGui::AlignTextToFramePadding();
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

	// eq eight
	ImGui::PushID("EQEight");
	if (ImGui::Selectable("EQ Eight")) {
	}
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("INTERNAL_PLUGIN", "EQEight", strlen("EQEight") + 1);
		ImGui::Text("EQ Eight");
		ImGui::TextDisabled("Effect");
		ImGui::EndDragDropSource();
	}
	ImGui::PopID();

	// analyzer
	ImGui::PushID("Analyzer");
	if (ImGui::Selectable("Analyzer")) {
	}
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("INTERNAL_PLUGIN", "Analyzer", strlen("Analyzer") + 1);
		ImGui::Text("Analyzer");
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

	// declare how tall the list comes out BEFORE the child begins. imgui decides a
	// child's scrollbar by measuring the content against the size that child had LAST
	// frame, unless the content size is spelled out - and folding the panel below
	// changes this column's height in one step, so without this the scrollbar trailed
	// the fold by a frame (and came back a frame late on the way out)
	int visiblePlugins = 0;
	for (const auto& plugin : plugins)
		if (filter.PassFilter(plugin.name.c_str()))
			++visiblePlugins;
	// one Selectable per plugin, each a text line tall with the item spacing under it.
	// imgui advances its cursor by a TRUNCATED row pitch, so truncate here as well: at a
	// fractional DPI scale the two differ by a pixel per row, and a list declared taller
	// than it draws can be scrolled into blank space past the last plugin
	const float rowPitch = std::floor(ImGui::GetTextLineHeightWithSpacing());
	const float listHeight = visiblePlugins > 0
								 ? (float)visiblePlugins * rowPitch - ImGui::GetStyle().ItemSpacing.y
								 : 0.0f;
	ImGui::SetNextWindowContentSize(ImVec2(0.0f, listHeight));

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
