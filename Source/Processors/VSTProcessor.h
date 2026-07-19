#pragma once
#include "AudioProcessor.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <set>

// include VST sdk
#include "pluginterfaces/vst2.x/aeffectx.h"

#if defined(_WIN32)
#include <windows.h>
#endif

class VSTProcessor : public AudioProcessor {
public:
	VSTProcessor(const std::string& path);
	~VSTProcessor() override;

	bool Load();

	const char* GetName() const override { return mName.c_str(); }
	std::string GetProcessorId() const override { return "VST"; }
	bool IsInstrument() const override { return mIsSynth; }
	const std::string& GetPath() const { return mPath; }

	void PrepareToPlay(double sampleRate) override;
	void Reset() override;
	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages,
				 const ProcessContext& context) override;

	bool HasEditor() const override;
	void OpenEditor(void* parentWindowHandle) override;
	void CloseEditor() override;
	bool IsEditorOpen() const override;
	void EditorIdle() override;

	void Save(std::ostream& out) override;
	void Load(std::istream& in) override;

	static VstIntPtr VSTCALLBACK HostCallback(AEffect* effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt);

	// static callback to route keyboard events from VST windows back to the editor
	static std::function<void(int virtualKey, bool isDown)> OnGlobalKeyEvent;
private:
	std::string mPath;
	std::string mName;
	bool mIsSynth = false;

#ifdef _WIN32
	HMODULE mLibrary = nullptr;
	static LRESULT CALLBACK EditorWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	HWND mEditorWindow = nullptr;
#else
	void* mLibrary = nullptr;
	void* mEditorWindow = nullptr;
#endif

	AEffect* mAEffect = nullptr;
	VstTimeInfo mTimeInfo;

	// parameter sync tracking
	std::vector<float> mLastSentValues;

	// audio buffers for de-interleaving
	std::vector<float> mProcessBuffer;
	std::vector<float*> mInputPtrs;
	std::vector<float*> mOutputPtrs;

	// MIDI
	VstEvents* mVSTEvents = nullptr;
	std::vector<uint8_t> mVSTEventBuffer;

	// track active MIDI notes per channel (16 channels) to ensure note-off on reset
	std::set<int> mActiveMIDINotes[16];

	void InitializeParameters();
	void Resume();
	void Suspend();
};
