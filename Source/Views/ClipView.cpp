#include "PrecompHeader.h"
#include "ClipView.h"
#include "Clips/AudioClip.h"
#include "Clips/MIDIClip.h"
#include "Project.h"
#include "Undo/Actions.h"
#include <cmath>
#include <string>
#include <algorithm>
#include <memory>
#include <mutex>

// ================================================================
// CLIP FIELD
// ================================================================

namespace {

	// a stepped field publishes whole steps only: a semitone drag that leaves the clip at
	// -0.4 st while the box rounds it to "-0" means what the user reads and what the clip
	// plays are two different numbers. round() also hands back a negative zero anywhere in
	// the lower half of the zero step, which "%.0f" prints as "-0" - the grid has one zero
	// and it is the positive one
	float SnapToStep(float value, float step) {
		if (step <= 0.0f)
			return value;
		float snapped = std::round(value / step) * step;
		if (snapped == 0.0f)
			snapped = 0.0f;
		return snapped;
	}

} // namespace

float AudioClipParameter::NormalizedFromValue() const {
	if (mStep <= 0.0f)
		return KnobParameter::NormalizedFromValue();

	const float range = maxValue - minValue;
	return range != 0.0f ? (mRawValue - minValue) / range : 0.0f;
}

void AudioClipParameter::SetValueFromNormalized(float t) {
	KnobParameter::SetValueFromNormalized(t);
	if (mStep > 0.0f) {
		mRawValue = value;
		value = SnapToStep(value, mStep);
	}
}

void AudioClipParameter::FormatValue(char* buffer, size_t bufferSize, const char* valueFmt) const {
	// the field's own format wins over whatever the widget asked for: a dial passes null
	// to let a knob variant choose, and none of the variants reads in semitones or cents
	ContinuousParameter::FormatValue(buffer, bufferSize, mValueFmt ? mValueFmt : valueFmt);
}

bool AudioClipParameter::DrawTracked(float width, float knobRadius) {
	if (!mEdit.clip) {
		mGestureActive = false;
		return false;
	}

	// the clip is the value, this object only mirrors it: read it back at the top of
	// every frame so an edit that came from anywhere else - another clip selected, an
	// undone warp action, a tempo change retiming the segment - is simply what the box
	// shows, with no sync to keep and nothing to drift
	const AudioClipWarpState frameBefore = mEdit.clip->CaptureWarpState();
	value = (float)((mEdit.clip.get()->*mRead)());
	// the drag accumulator only has to outlive the frames of one drag; outside a gesture
	// it tracks the clip, so the next drag starts from wherever the value actually is
	if (!IsInEditGesture())
		mRawValue = value;

	mCommitPending = false;
	const bool changed = knobRadius > 0.0f
							 ? DrawSized(knobRadius, width)
							 : DrawCompact(width > 0.0f ? width : ImGui::GetContentRegionAvail().x, mValueFmt);

	// a drag that began this frame has not moved the clip yet, so the state read above
	// is the one the whole gesture undoes back to
	if (!mGestureActive && IsInEditGesture()) {
		mGestureActive = true;
		mGestureBefore = frameBefore;
	}

	if (changed) {
		// a typed value and a double-click reset land straight on `value`, never going
		// through the snap a drag already passed through
		value = SnapToStep(value, mStep);
		WriteToClip();
	}

	// pushed from here rather than from CommitEdit because this is the only point that
	// sees the clip AFTER the edit has landed on it
	if (mCommitPending) {
		if (mEdit.project && mEdit.undoManager)
			mEdit.undoManager->Push(std::make_unique<AudioClipWarpAction>(
				mEdit.project, mEdit.clip,
				mGestureActive ? mGestureBefore : frameBefore,
				mEdit.clip->CaptureWarpState(), mActionName));
		mGestureActive = false;
	} else if (!IsInEditGesture()) {
		mGestureActive = false;
	}

	return changed;
}

void AudioClipParameter::WriteToClip() {
	AudioClip* clip = mEdit.clip.get();
	auto apply = [&]() {
		// the warp fields decide how many beats of the file the clip covers, so the edit
		// is not finished until the clip's window has been re-read from the new reach.
		// doing it unconditionally keeps that in one place and costs the fields that do
		// not move it an exact scale of one
		double maxBefore = clip->GetMaxDurationInBeats(mEdit.projectBpm);
		(clip->*mWrite)((double)value);
		clip->RetimeForWarpChange(maxBefore, mEdit.projectBpm);
	};

	// the audio thread reads all of this while it renders the clip
	if (mEdit.project) {
		std::lock_guard<std::mutex> lock(mEdit.project->GetMutex());
		apply();
	} else {
		apply();
	}
}

void AudioClipParameter::CommitEdit(float, float) {
	// deliberately does not chain to the base: the undo entry is an AudioClipWarpAction
	// pushed by DrawTracked, and a clip field has no automation lane to be the Show Auto
	// button's "last touched parameter" for
	mCommitPending = true;
}

// ================================================================
// CLIP VIEW
// ================================================================

ClipView::ClipView(EditorContext& context)
	: mContext(context),
	  // the ranges are the ones the drag fields carried before; a value box travels its
	  // whole range over 200 px, with Shift for a tenth of that and digits typed straight
	  // in when neither is fine enough
	  mSegmentBpm(mEdit, "Seg. BPM", 120.0f, 20.0f, 999.0f,
				  &AudioClip::GetSegmentBpm, &AudioClip::SetSegmentBpm, "%.2f", "Set clip BPM"),
	  mTransientEnvelope(mEdit, "Envelope", 0.5f, 0.0f, 1.0f,
						 &AudioClip::GetTransientEnvelope, &AudioClip::SetTransientEnvelope, "%.2f", "Set transient envelope"),
	  mGrainSize(mEdit, "Grain Size", 80.0f, 20.0f, 300.0f,
				 &AudioClip::GetGrainSizeMs, &AudioClip::SetGrainSizeMs, "%.0f ms", "Set grain size"),
	  mFluctuation(mEdit, "Fluctuation", 0.0f, 0.0f, 1.0f,
				   &AudioClip::GetFluctuation, &AudioClip::SetFluctuation, "%.2f", "Set fluctuation"),
	  mFormants(mEdit, "Formants", 1.0f, 0.0f, 1.0f,
				&AudioClip::GetFormants, &AudioClip::SetFormants, "%.2f", "Set formants"),
	  // the two pitch fields step in whole units, which is both what they read in and the
	  // only way "%.0f" can print what the clip is actually playing
	  mTransposeSemitones(mEdit, "Semitones", 0.0f, -48.0f, 48.0f,
						  &AudioClip::GetTransposeSemitones, &AudioClip::SetTransposeSemitones, "%.0f st", "Transpose clip",
						  1.0f, ImGuiKnobVariant_LinearBipolar),
	  mTransposeCents(mEdit, "Detune", 0.0f, -100.0f, 100.0f,
					  &AudioClip::GetTransposeCents, &AudioClip::SetTransposeCents, "%.0f ct", "Transpose clip", 1.0f) {}

void ClipView::Render(const ImVec2& pos, float width, float height) {
	ImVec2 defaultPadding = ImGui::GetStyle().WindowPadding;
	// prevent double-padding issues with full-size children
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::Begin("Clip View", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

	// restore padding for content
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, defaultPadding);
	ImGui::BeginChild("ClipContent", ImVec2(width, height), false, ImGuiWindowFlags_None);
	ImGui::PopStyleVar();

	auto clip = mContext.state.selectedClip;

	if (!clip) {
		float txtW = ImGui::CalcTextSize("No Clip Selected").x;
		ImGui::SetCursorPos(ImVec2(width * 0.5f - txtW * 0.5f, height * 0.5f - 10.0f));
		ImGui::TextDisabled("No Clip Selected");
		mEdit.clip = nullptr;
		ImGui::EndChild();
		ImGui::End();
		ImGui::PopStyleVar();
		return;
	}

	// basic info header
	ImGui::Text("Clip: %s", clip->GetName().c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("(%.2f beats)", clip->GetDuration());
	ImGui::SameLine();

	// activation toggle. the undo step snapshots the owning track, so it needs the
	// selected track rather than the clip alone
	bool clipEnabled = clip->IsEnabled();
	if (ImGui::Checkbox("Active", &clipEnabled)) {
		Project* clipProject = mContext.GetProject();
		std::shared_ptr<Track> ownerTrack = nullptr;
		if (clipProject) {
			auto& tracks = clipProject->GetTracks();
			int idx = mContext.state.selectedTrackIndex;
			if (idx >= 0 && idx < (int)tracks.size())
				ownerTrack = tracks[idx];
		}
		ToggleClipEnabled(clipProject, mContext.undoManager, ownerTrack, clip);
	}
	ImGui::Separator();

	auto ac = std::dynamic_pointer_cast<AudioClip>(clip);
	// the fields read the clip through this, so it is refilled before any of them draw
	// and emptied again the moment a MIDI clip (or nothing) is selected
	mEdit.project = mContext.GetProject();
	mEdit.undoManager = &mContext.undoManager;
	mEdit.clip = ac;
	mEdit.projectBpm = mEdit.project ? mEdit.project->GetTransport().GetBpm() : 120.0;

	if (ac) {
		Project* project = mEdit.project;
		const double projectBpm = mEdit.projectBpm;

		// the toggles and buttons are not parameters, so they take the lock and push
		// their own before -> after step by hand. the geometry re-read rides along for the
		// same reason it does in AudioClipParameter::WriteToClip: an edit that changes how
		// much of the file fits in a beat is not finished until the clip's window has been
		// re-read from it, and having one place decide that is what keeps a pitch drag and
		// the /2 button resizing the clip the same way
		auto locked = [&](auto&& fn) {
			auto apply = [&]() {
				double maxBefore = ac->GetMaxDurationInBeats(projectBpm);
				fn();
				ac->RetimeForWarpChange(maxBefore, projectBpm);
			};
			if (project) {
				std::lock_guard<std::mutex> lock(project->GetMutex());
				apply();
			} else {
				apply();
			}
		};
		auto pushUndo = [&](const AudioClipWarpState& before, const char* name) {
			if (project)
				mContext.undoManager.Push(std::make_unique<AudioClipWarpAction>(project, ac, before, ac->CaptureWarpState(), name));
		};

		// a label in a fixed column with the value box beside it, which is how the
		// mixer and the transport lay their parameters out too
		const float fieldWidth = 100.0f * mContext.state.mainScale;
		const float labelWidth = ImGui::CalcTextSize("Fluctuation").x + ImGui::GetStyle().ItemSpacing.x;
		auto field = [&](AudioClipParameter& param, const char* tooltip) {
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(param.name.c_str());
			ImGui::SameLine(labelWidth);
			param.DrawField(fieldWidth);
			if (tooltip && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", tooltip);
		};

		// audio clip controls
		ImGui::Columns(3, "AudioClipCols", false);

		// column 1: warping
		{
			ImGui::Text("Warping");
			ImGui::Dummy(ImVec2(0, 5));

			bool warping = ac->IsWarpingEnabled();
			if (ImGui::Checkbox("Warp", &warping)) {
				AudioClipWarpState before = ac->CaptureWarpState();
				locked([&]() {
					ac->SetWarpingEnabled(warping);
					// enabling warp anchors the clip to the grid at the current tempo:
					// segment bpm := project bpm makes the warp ratio exactly 1, so there
					// is no speed/pitch jump at that instant
					if (warping)
						ac->SetSegmentBpm(projectBpm);
				});
				pushUndo(before, warping ? "Enable warp" : "Disable warp");
			}

			if (warping) {
				const char* warpModes[] = {"Beats", "Tones", "Texture", "Re-Pitch", "Complex", "Complex Pro"};
				int currentMode = (int)ac->GetWarpMode();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted("Mode");
				ImGui::SameLine(labelWidth);
				ImGui::SetNextItemWidth(fieldWidth);
				if (ImGui::Combo("##WarpMode", &currentMode, warpModes, IM_ARRAYSIZE(warpModes))) {
					if (currentMode >= 0 && currentMode <= 5) {
						AudioClipWarpState before = ac->CaptureWarpState();
						locked([&]() { ac->SetWarpMode((WarpMode)currentMode); });
						pushUndo(before, "Change warp mode");
					}
				}

				field(mSegmentBpm, "Tempo the file was recorded at");

				// half/double-time: the usual fix when the detected tempo is an octave off
				ImGui::SameLine();
				if (ImGui::SmallButton("/2")) {
					AudioClipWarpState before = ac->CaptureWarpState();
					locked([&]() {
						ac->SetSegmentBpm(ac->GetSegmentBpm() * 0.5);
					});
					pushUndo(before, "Halve clip BPM");
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("x2")) {
					AudioClipWarpState before = ac->CaptureWarpState();
					locked([&]() {
						ac->SetSegmentBpm(ac->GetSegmentBpm() * 2.0);
					});
					pushUndo(before, "Double clip BPM");
				}

				// per-mode controls, mirroring Ableton's per-warp-mode knobs
				WarpMode mode = ac->GetWarpMode();
				if (mode == WarpMode::Beats) {
					field(mTransientEnvelope, "How hard each transient's grain is faded");
				} else if (mode != WarpMode::RePitch) {
					// Tones/Texture/Complex/ComplexPro share a grain-size knob
					field(mGrainSize, "Length of the grains the file is rebuilt from");

					if (mode == WarpMode::Texture)
						field(mFluctuation, "Randomizes the grain positions");
					if (mode == WarpMode::ComplexPro)
						field(mFormants, "How much of the original formants is kept");
				}

				// honest one-liner: only Re-Pitch couples tempo to pitch
				if (mode == WarpMode::RePitch)
					ImGui::TextDisabled("Tempo change shifts pitch (tape)");
				else
					ImGui::TextDisabled("Pitch preserved on tempo change");
			} else {
				ImGui::TextDisabled("Warping Disabled");
				ImGui::TextDisabled("Audio plays at native speed");
			}
		}
		ImGui::NextColumn();

		// column 2: pitch
		{
			ImGui::Text("Pitch / Transpose");
			ImGui::Dummy(ImVec2(0, 5));

			// Re-Pitch derives pitch from playback speed, so transpose is meaningless there
			// (Ableton disables it too); grey the controls out while that mode is active
			bool repitch = ac->IsWarpingEnabled() && ac->GetWarpMode() == WarpMode::RePitch;
			if (repitch)
				ImGui::TextDisabled("Disabled in Re-Pitch");
			ImGui::BeginDisabled(repitch);

			// the transpose is the control an arrangement is actually tuned from, so it
			// gets the dial and the boxes are left to the trim under it. sized to the
			// same block the label/value rows below occupy, so the two line up
			mTransposeSemitones.DrawKnob(KnobParameter::kDefaultRadius * mContext.state.mainScale, labelWidth + fieldWidth);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Transpose in whole semitones");
			field(mTransposeCents, "Fine tuning, a hundredth of a semitone each");

			// add a helper reset button
			if (ImGui::Button("Reset Pitch")) {
				AudioClipWarpState before = ac->CaptureWarpState();
				locked([&]() {
					ac->SetTransposeSemitones(0.0);
					ac->SetTransposeCents(0.0);
				});
				pushUndo(before, "Reset pitch");
			}

			ImGui::EndDisabled();
		}
		ImGui::NextColumn();

		// column 3: file info
		{
			ImGui::TextDisabled("File Info");
			ImGui::Dummy(ImVec2(0, 5));
			ImGui::Text("Sample Rate: %.0f Hz", ac->GetSampleRate());
			ImGui::Text("Channels: %d", ac->GetNumChannels());
			double durSec = 0.0;
			if (ac->GetSampleRate() > 0)
				durSec = (double)ac->GetTotalFileFrames() / ac->GetSampleRate();
			ImGui::Text("Length: %.2f sec", durSec);
			ImGui::Text("Frames: %llu", ac->GetTotalFileFrames());
		}

		ImGui::Columns(1);
	} else if (auto mc = std::dynamic_pointer_cast<MIDIClip>(clip)) {
		// MIDI clip controls
		ImGui::Text("MIDI Properties");
		ImGui::Separator();
		ImGui::Text("Notes: %zu", mc->GetNotes().size());
		// placeholder for MIDI specific tools
		ImGui::TextDisabled("Use Piano Roll to edit notes.");
	}

	ImGui::EndChild();
	ImGui::End();
	ImGui::PopStyleVar();
}
