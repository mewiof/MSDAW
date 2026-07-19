#pragma once
#include <vector>
#include <string>
#include <memory>
#include <iostream>
#include <iomanip>
#include "MIDITypes.h"
#include "Parameter.h"
#include "AppConfig.h"

// how a plugin's editor window handles high-DPI displays. Default follows the
// global AppConfig setting; the other two force a specific behavior per plugin
enum class EditorScalingMode {
	Default = 0, // use the global default (AppConfig::pluginEditorsNative)
	Native = 1,	 // DPI-aware: crisp, native-resolution rendering
	Scaled = 2	 // DPI-unaware: Windows stretches to match the DAW (may blur)
};

// process context with transport information
struct ProcessContext {
	double sampleRate = 48000.0;
	int64_t currentSample = 0; // samples relative to project start
	double bpm = 120.0;
	bool isPlaying = false;
	double timeSigNumerator = 4.0;
	double timeSigDenominator = 4.0;
	// true only on a block whose start is a fresh (re)start or seek of the playhead;
	// lets the sequencer chase note onsets that round to just before the block start so
	// a note lined up with the playhead still fires. must stay false during contiguous
	// playback, or notes landing on a block boundary would double-trigger
	bool playheadJumped = false;
};

// base class for audio processors
class AudioProcessor {
public:
	virtual ~AudioProcessor() = default;

	// ui display name
	virtual const char* GetName() const { return "Audio Processor"; }

	// serialization id
	virtual std::string GetProcessorId() const { return "Unknown"; }

	// check if processor is instrument
	virtual bool IsInstrument() const { return false; }

	// setup
	virtual void PrepareToPlay(double sampleRate) = 0;

	// reset state
	virtual void Reset() {}

	// process block
	virtual void Process(float* buffer, int numFrames, int numChannels,
						 std::vector<MIDIMessage>& mIDIMessages,
						 const ProcessContext& context) = 0;

	// get parameters
	const std::vector<std::unique_ptr<Parameter>>& GetParameters() const { return mParameters; }

	// bypass state
	bool IsBypassed() const { return mIsBypassed; }
	void SetBypassed(bool bypassed) { mIsBypassed = bypassed; }

	// VST editor support
	virtual bool HasEditor() const { return false; }

	// open editor with window handle
	virtual void OpenEditor(void* parentWindowHandle = nullptr) { (void)parentWindowHandle; }

	// close the native editor window (no-op for processors without one)
	virtual void CloseEditor() {}

	// whether the native editor window is currently open
	virtual bool IsEditorOpen() const { return false; }

	// called once per UI frame while open so editors that need periodic servicing
	// (e.g. VST2 effEditIdle) can repaint. no-op by default
	virtual void EditorIdle() {}

	// per-plugin high-DPI override for the editor window
	EditorScalingMode GetEditorScalingMode() const { return mEditorScalingMode; }
	void SetEditorScalingMode(EditorScalingMode mode) { mEditorScalingMode = mode; }

	// resolve the override + global default into a concrete decision
	bool UseNativeEditorScaling() const {
		switch (mEditorScalingMode) {
		case EditorScalingMode::Native:
			return true;
		case EditorScalingMode::Scaled:
			return false;
		case EditorScalingMode::Default:
		default:
			return AppConfig::Instance().pluginEditorsNative;
		}
	}

	virtual bool RenderCustomUI(const ImVec2& size) {
		(void)size;
		return false;
	}

	// serialization
	virtual void Save(std::ostream& out) {
		out << "PARAMS_BEGIN\n";
		for (const auto& p : mParameters) {
			out << "P \"" << p->name << "\" " << p->value << "\n";
		}
		out << "PARAMS_END\n";
	}

	virtual void Load(std::istream& in) {
		std::string line;
		while (std::getline(in, line)) {
			if (line == "PARAMS_END")
				break;
			if (line.rfind("P ", 0) == 0) {
				size_t q1 = line.find('"');
				size_t q2 = line.find('"', q1 + 1);
				if (q1 != std::string::npos && q2 != std::string::npos) {
					std::string pName = line.substr(q1 + 1, q2 - q1 - 1);
					std::string valStr = line.substr(q2 + 1);
					float val = std::stof(valStr);

					for (auto& p : mParameters) {
						if (p->name == pName) {
							p->value = val;
							break;
						}
					}
				}
			}
		}
	}
protected:
	std::vector<std::unique_ptr<Parameter>> mParameters;
	bool mIsBypassed = false;
	EditorScalingMode mEditorScalingMode = EditorScalingMode::Default;

	template <typename T>
	T* AddParameter(std::unique_ptr<T> parameter) {
		T* p = parameter.get();
		mParameters.push_back(std::move(parameter));
		return p;
	}
};
