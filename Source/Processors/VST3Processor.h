#pragma once
#include "AudioProcessor.h"
#include <string>
#include <vector>
#include <memory>
#include <set>
#include "PluginManager.h"

#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/base/ibstream.h"

#if defined(_WIN32)
#include <windows.h>
#endif

class VST3HostContext : public Steinberg::Vst::IHostApplication {
public:
	VST3HostContext() = default;
	virtual ~VST3HostContext() = default;

	static Steinberg::Vst::IHostApplication* GetInstance();
	static void Shutdown();

	Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override;
	Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID cid, Steinberg::TUID _iid, void** obj) override;

	DECLARE_FUNKNOWN_METHODS
private:
	static Steinberg::Vst::IHostApplication* sInstance;
	Steinberg::uint32 mRefCount = 1;
};

class VST3ComponentHandler : public Steinberg::Vst::IComponentHandler {
public:
	VST3ComponentHandler(AudioProcessor* processor) : mProcessor(processor) {}

	Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override {
		// plugin GUI started a parameter gesture: capture the value for undo
		if (mProcessor) {
			const auto& params = mProcessor->GetParameters();
			if (id < params.size())
				params[id]->BeginEditGesture();
		}
		return Steinberg::kResultTrue;
	}
	Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) override;
	Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override {
		// plugin GUI finished the gesture: commit one undo entry
		if (mProcessor) {
			const auto& params = mProcessor->GetParameters();
			if (id < params.size())
				params[id]->EndEditGesture();
		}
		return Steinberg::kResultTrue;
	}
	Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override { return Steinberg::kResultTrue; }

	DECLARE_FUNKNOWN_METHODS
private:
	AudioProcessor* mProcessor;
	Steinberg::uint32 mRefCount = 1;
};

class VST3Processor : public AudioProcessor {
public:
	VST3Processor(const std::string& path, const std::string& classID = "");
	~VST3Processor() override;

	bool Load();

	const char* GetName() const override { return mName.c_str(); }
	std::string GetProcessorId() const override { return "VST3"; }
	bool IsInstrument() const override { return mIsSynth; }
	const std::string& GetPath() const { return mPath; }
	const std::string& GetClassID() const { return mClassID; }

	void PrepareToPlay(double sampleRate) override;
	void Reset() override;
	void AllNotesOff() override;
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

	static std::vector<PluginInfo> EnumeratePlugins(const std::string& path);
private:
	std::string mPath;
	std::string mClassID;
	std::string mName;
	bool mIsSynth = false;

	void* mModuleHandle = nullptr;
	Steinberg::IPluginFactory* mFactory = nullptr;
	Steinberg::Vst::IComponent* mComponent = nullptr;
	Steinberg::Vst::IAudioProcessor* mProcessor = nullptr;
	Steinberg::Vst::IEditController* mController = nullptr;
	Steinberg::IPlugView* mPlugView = nullptr;
	Steinberg::IPlugFrame* mPlugFrame = nullptr;
	VST3ComponentHandler* mComponentHandler = nullptr;

	std::vector<float> mProcessBuffer;
	std::vector<float*> mInputPtrs;
	std::vector<float*> mOutputPtrs;

	struct EventList;
	struct ParameterChanges;

	std::unique_ptr<EventList> mEventList;
	std::unique_ptr<ParameterChanges> mParamChanges;

	std::set<int> mActiveMIDINotes[16];

	std::vector<float> mLastSentValues;

	double mSampleRate = 48000.0;
	bool mIsActive = false;
	bool mNeedsFlush = false;

#ifdef _WIN32
	HWND mEditorWindow = nullptr;
	static LRESULT CALLBACK EditorWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

	void InitializeParameters();
	void SetupBuses(int numChannels);
	void SyncParametersToController(int numFrames);
	void ConvertMIDIToEvents(std::vector<MIDIMessage>& midiMessages);
};
