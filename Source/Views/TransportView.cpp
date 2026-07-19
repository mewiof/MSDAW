#include "PrecompHeader.h"
#include "TransportView.h"
#include "Project.h"
#include "Transport.h"
#include "Parameter.h"
#include "Theme.h"

void TransportView::Render(const ImVec2& pos, float width, float height) {
	const Theme& th = Theme::Instance();
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::Begin("Transport", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

	Project* project = mContext.GetProject();
	Transport* transport = project ? &project->GetTransport() : nullptr;

	if (transport) {
		bool isPlaying = transport->IsPlaying();
		if (ImGui::Button(isPlaying ? " || " : " > ", ImVec2(40 * mContext.state.mainScale, 0))) {
			if (isPlaying) {
				// pause logic (ableton style): stop and return to insert marker
				transport->Pause();
				double startBeat = mContext.state.selectionStart;
				int64_t startSample = (int64_t)(startBeat * (60.0 / transport->GetBpm()) * transport->GetSampleRate());
				transport->SetPosition(startSample);
			} else {
				// play logic (ableton style): play from insert marker
				double startBeat = mContext.state.selectionStart;
				int64_t startSample = (int64_t)(startBeat * (60.0 / transport->GetBpm()) * transport->GetSampleRate());
				transport->SetPosition(startSample);
				transport->Play();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Stop", ImVec2(40 * mContext.state.mainScale, 0)))
			transport->Stop(); // resets to 0
		ImGui::SameLine();

		// loop toggle
		bool isLooping = transport->IsLoopEnabled();
		if (isLooping) {
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accentHover);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}
		if (ImGui::Button("Loop", ImVec2(40 * mContext.state.mainScale, 0))) {
			transport->SetLoopEnabled(!isLooping);
		}
		if (isLooping)
			ImGui::PopStyleColor(3);

		ImGui::SameLine();

		// follow toggle
		bool follow = mContext.state.followPlayback;
		if (follow) {
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accentHover);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}
		if (ImGui::Button("Follow", ImVec2(50 * mContext.state.mainScale, 0))) {
			mContext.state.followPlayback = !follow;
		}
		if (follow)
			ImGui::PopStyleColor(3);

		ImGui::SameLine();
		ImGui::Text("| Time: %.2f", (double)transport->GetPosition() / transport->GetSampleRate());
		ImGui::SameLine();

		// bpm
		float bpm = (float)transport->GetBpm();
		ImGui::SetNextItemWidth(60 * mContext.state.mainScale);
		if (ImGui::DragFloat("BPM", &bpm, 1.0f, 40.0f, 300.0f, "%.1f"))
			if (mContext.GetProject()) {
				mContext.GetProject()->SetBpm(bpm);
			}

		ImGui::SameLine();

		ImGui::Text("Grid");
		ImGui::SameLine();

		// numerator
		ImGui::SetNextItemWidth(30 * mContext.state.mainScale);
		bool gridChanged = false;
		if (ImGui::DragInt("##GridNum", &mContext.state.timelineGridNumerator, 0.2f, 1, 64))
			gridChanged = true;

		ImGui::SameLine();
		ImGui::Text("/");
		ImGui::SameLine();

		// denominator
		ImGui::SetNextItemWidth(30 * mContext.state.mainScale);
		if (ImGui::DragInt("##GridDen", &mContext.state.timelineGridDenominator, 0.2f, 1, 128))
			gridChanged = true;

		// update
		if (gridChanged) {
			if (mContext.state.timelineGridNumerator < 1)
				mContext.state.timelineGridNumerator = 1;
			if (mContext.state.timelineGridDenominator < 1)
				mContext.state.timelineGridDenominator = 1;
			mContext.state.timelineGrid = (double)mContext.state.timelineGridNumerator / (double)mContext.state.timelineGridDenominator;
		}

		// TODO: remove this
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Grid value: %.4f", mContext.state.timelineGrid);

		// computer MIDI keyboard toggle
		ImGui::SameLine();
		ImGui::Dummy(ImVec2(20, 0)); // spacer
		ImGui::SameLine();

		bool mIDIKey = mContext.state.isComputerMIDIKeyboardEnabled;
		if (mIDIKey) {
			// amber active state (ableton-like), dark text for contrast on the accent
			ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accentHover);
			ImGui::PushStyleColor(ImGuiCol_Text, th.textOnAccent);
		}

		if (ImGui::Button("KEY", ImVec2(40 * mContext.state.mainScale, 0))) {
			mContext.state.isComputerMIDIKeyboardEnabled = !mIDIKey;
		}

		if (mIDIKey) {
			ImGui::PopStyleColor(3);
		}

		// optional: show current octave/vel on hover or beside it
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Computer MIDI Keyboard (M)\nOctave: %d (Z/X)\nVelocity: %d (C/V)",
							  mContext.state.mIDIOctave, mContext.state.mIDIVelocity);
		}

		// jump to the automation lane of the last edited knob/parameter
		ImGui::SameLine();
		Parameter* lastParam = Parameter::GetLastTouchedParameter();
		ImGui::BeginDisabled(lastParam == nullptr);
		if (ImGui::Button("Show Auto", ImVec2(80 * mContext.state.mainScale, 0)) && lastParam)
			Parameter::RequestAutomation(lastParam);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) {
			if (lastParam)
				ImGui::SetTooltip("Show automation for last edited parameter:\n%s", lastParam->name.c_str());
			else
				ImGui::SetTooltip("Show automation for the last edited parameter\n(edit a knob or slider first)");
		}
	}
	ImGui::SameLine(ImGui::GetWindowWidth() - 150 * mContext.state.mainScale);
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(mContext.engine.IsStreamRunning() ? th.success : th.danger),
					   mContext.engine.IsStreamRunning() ? "Audio: ON" : "Audio: OFF");
	ImGui::End();
}
