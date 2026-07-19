#include "Parameters/SliderParameter.h"
#include "PrecompHeader.h"

// define INIT_CLASS_IID before including the VST3 headers for GUIDs
#define INIT_CLASS_IID
#include "VST3Processor.h"
#undef INIT_CLASS_IID

#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>

#if defined(_WIN32)
#include <windows.h>
#endif

// base64 helper for chunk serialization
static const std::string base64_chars =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789+/";

static std::string Base64Encode(const unsigned char* bytes_to_encode, unsigned int in_len) {
	std::string ret;
	int i = 0, j = 0;
	unsigned char char_array_3[3], char_array_4[4];

	while (in_len--) {
		char_array_3[i++] = *(bytes_to_encode++);
		if (i == 3) {
			char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
			char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
			char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
			char_array_4[3] = char_array_3[2] & 0x3f;
			for (i = 0; (i < 4); i++)
				ret += base64_chars[char_array_4[i]];
			i = 0;
		}
	}
	if (i) {
		for (j = i; j < 3; j++)
			char_array_3[j] = '\0';
		char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
		char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
		char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
		char_array_4[3] = char_array_3[2] & 0x3f;
		for (j = 0; (j < i + 1); j++)
			ret += base64_chars[char_array_4[j]];
		while (i++ < 3)
			ret += '=';
	}
	return ret;
}

static std::vector<unsigned char> Base64Decode(const std::string& encoded_string) {
	int in_len = (int)encoded_string.size();
	int i = 0, j = 0, in_ = 0;
	unsigned char char_array_4[4], char_array_3[3];
	std::vector<unsigned char> ret;

	auto is_base64 = [](unsigned char c) { return (isalnum(c) || (c == '+') || (c == '/')); };

	while (in_len-- && (encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
		char_array_4[i++] = encoded_string[in_];
		in_++;
		if (i == 4) {
			for (i = 0; i < 4; i++)
				char_array_4[i] = (unsigned char)base64_chars.find(char_array_4[i]);
			char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
			char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
			char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
			for (i = 0; (i < 3); i++)
				ret.push_back(char_array_3[i]);
			i = 0;
		}
	}
	if (i) {
		for (j = i; j < 4; j++)
			char_array_4[j] = 0;
		for (j = 0; j < 4; j++)
			char_array_4[j] = (unsigned char)base64_chars.find(char_array_4[j]);
		char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
		char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
		char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
		for (j = 0; (j < i - 1); j++)
			ret.push_back(char_array_3[j]);
	}
	return ret;
}

class MemoryStream : public Steinberg::IBStream {
public:
	MemoryStream() {}
	MemoryStream(const void* data, Steinberg::int32 size) {
		mBuffer.assign((const uint8_t*)data, ((const uint8_t*)data) + size);
	}
	virtual ~MemoryStream() = default;

	Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override {
		Steinberg::int32 available = (Steinberg::int32)mBuffer.size() - mPos;
		Steinberg::int32 toRead = (std::min)(numBytes, available);
		if (toRead > 0) {
			std::memcpy(buffer, mBuffer.data() + mPos, toRead);
			mPos += toRead;
		}
		if (numBytesRead)
			*numBytesRead = toRead;
		return Steinberg::kResultTrue;
	}

	Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten) override {
		if (mPos + numBytes > (Steinberg::int32)mBuffer.size()) {
			mBuffer.resize(mPos + numBytes);
		}
		std::memcpy(mBuffer.data() + mPos, buffer, numBytes);
		mPos += numBytes;
		if (numBytesWritten)
			*numBytesWritten = numBytes;
		return Steinberg::kResultTrue;
	}

	Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result) override {
		if (mode == Steinberg::IBStream::kIBSeekSet)
			mPos = (Steinberg::int32)pos;
		else if (mode == Steinberg::IBStream::kIBSeekCur)
			mPos += (Steinberg::int32)pos;
		else if (mode == Steinberg::IBStream::kIBSeekEnd)
			mPos = (Steinberg::int32)mBuffer.size() + (Steinberg::int32)pos;

		if (mPos < 0)
			mPos = 0;
		if (mPos > (Steinberg::int32)mBuffer.size())
			mPos = (Steinberg::int32)mBuffer.size();

		if (result)
			*result = mPos;
		return Steinberg::kResultTrue;
	}

	Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
		if (pos)
			*pos = mPos;
		return Steinberg::kResultTrue;
	}

	const std::vector<uint8_t>& getData() const { return mBuffer; }

	DECLARE_FUNKNOWN_METHODS
private:
	std::vector<uint8_t> mBuffer;
	Steinberg::int32 mPos = 0;
	Steinberg::uint32 mRefCount = 1;
};

Steinberg::tresult PLUGIN_API MemoryStream::queryInterface(const Steinberg::TUID _iid, void** obj) {
	QUERY_INTERFACE(_iid, obj, Steinberg::IBStream::iid, Steinberg::IBStream)
	*obj = nullptr;
	return Steinberg::kNoInterface;
}
Steinberg::uint32 PLUGIN_API MemoryStream::addRef() {
	return ++mRefCount;
}
Steinberg::uint32 PLUGIN_API MemoryStream::release() {
	if (--mRefCount == 0) {
		delete this;
		return 0;
	}
	return mRefCount;
}

struct VST3Processor::EventList : public Steinberg::Vst::IEventList {
	std::vector<Steinberg::Vst::Event> events;
	Steinberg::uint32 mRefCount = 1;

	Steinberg::int32 PLUGIN_API getEventCount() override { return (Steinberg::int32)events.size(); }
	Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index, Steinberg::Vst::Event& e) override {
		if (index >= 0 && index < (Steinberg::int32)events.size()) {
			e = events[index];
			return Steinberg::kResultTrue;
		}
		return Steinberg::kResultFalse;
	}
	Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event& e) override {
		events.push_back(e);
		return Steinberg::kResultTrue;
	}
	void clear() { events.clear(); }

	DECLARE_FUNKNOWN_METHODS
};
Steinberg::tresult PLUGIN_API VST3Processor::EventList::queryInterface(const Steinberg::TUID _iid, void** obj) {
	QUERY_INTERFACE(_iid, obj, Steinberg::Vst::IEventList::iid, Steinberg::Vst::IEventList)
	*obj = nullptr;
	return Steinberg::kNoInterface;
}
Steinberg::uint32 PLUGIN_API VST3Processor::EventList::addRef() {
	return ++mRefCount;
}
Steinberg::uint32 PLUGIN_API VST3Processor::EventList::release() {
	return --mRefCount;
}

class ParamValueQueue : public Steinberg::Vst::IParamValueQueue {
public:
	Steinberg::Vst::ParamID id;
	struct Point {
		Steinberg::int32 offset;
		Steinberg::Vst::ParamValue value;
	};
	std::vector<Point> points;
	Steinberg::uint32 mRefCount = 1;

	Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return id; }
	Steinberg::int32 PLUGIN_API getPointCount() override { return (Steinberg::int32)points.size(); }
	Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32& offset, Steinberg::Vst::ParamValue& value) override {
		if (index >= 0 && index < (Steinberg::int32)points.size()) {
			offset = points[index].offset;
			value = points[index].value;
			return Steinberg::kResultTrue;
		}
		return Steinberg::kResultFalse;
	}
	Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 offset, Steinberg::Vst::ParamValue value, Steinberg::int32& index) override {
		points.push_back({offset, value});
		index = (Steinberg::int32)points.size() - 1;
		return Steinberg::kResultTrue;
	}

	DECLARE_FUNKNOWN_METHODS
};
Steinberg::tresult PLUGIN_API ParamValueQueue::queryInterface(const Steinberg::TUID _iid, void** obj) {
	QUERY_INTERFACE(_iid, obj, Steinberg::Vst::IParamValueQueue::iid, Steinberg::Vst::IParamValueQueue)
	*obj = nullptr;
	return Steinberg::kNoInterface;
}
Steinberg::uint32 PLUGIN_API ParamValueQueue::addRef() {
	return ++mRefCount;
}
Steinberg::uint32 PLUGIN_API ParamValueQueue::release() {
	if (--mRefCount == 0) {
		delete this;
		return 0;
	}
	return mRefCount;
}

struct VST3Processor::ParameterChanges : public Steinberg::Vst::IParameterChanges {
	std::vector<ParamValueQueue*> queues;
	Steinberg::uint32 mRefCount = 1;

	~ParameterChanges() { clear(); }
	void clear() {
		for (auto q : queues)
			q->release();
		queues.clear();
	}

	Steinberg::int32 PLUGIN_API getParameterCount() override { return (Steinberg::int32)queues.size(); }
	Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override {
		if (index >= 0 && index < (Steinberg::int32)queues.size())
			return queues[index];
		return nullptr;
	}
	Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID& id, Steinberg::int32& index) override {
		for (size_t i = 0; i < queues.size(); ++i) {
			if (queues[i]->id == id) {
				index = (Steinberg::int32)i;
				return queues[i];
			}
		}
		auto* q = new ParamValueQueue();
		q->id = id;
		queues.push_back(q);
		index = (Steinberg::int32)queues.size() - 1;
		return q;
	}

	DECLARE_FUNKNOWN_METHODS
};
Steinberg::tresult PLUGIN_API VST3Processor::ParameterChanges::queryInterface(const Steinberg::TUID _iid, void** obj) {
	QUERY_INTERFACE(_iid, obj, Steinberg::Vst::IParameterChanges::iid, Steinberg::Vst::IParameterChanges)
	*obj = nullptr;
	return Steinberg::kNoInterface;
}
Steinberg::uint32 PLUGIN_API VST3Processor::ParameterChanges::addRef() {
	return ++mRefCount;
}
Steinberg::uint32 PLUGIN_API VST3Processor::ParameterChanges::release() {
	return --mRefCount;
}

//--- VST3HostContext ------------------------------------------------------

Steinberg::Vst::IHostApplication* VST3HostContext::sInstance = nullptr;

Steinberg::Vst::IHostApplication* VST3HostContext::GetInstance() {
	if (!sInstance) {
		sInstance = new VST3HostContext();
	}
	return sInstance;
}

void VST3HostContext::Shutdown() {
	if (sInstance) {
		sInstance->release();
		sInstance = nullptr;
	}
}

Steinberg::tresult PLUGIN_API VST3HostContext::getName(Steinberg::Vst::String128 name) {
	std::string hostName = "MSDAW Host";
	for (size_t i = 0; i < hostName.length() && i < 127; ++i)
		name[i] = hostName[i];
	name[hostName.length()] = 0;
	return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API VST3HostContext::createInstance(Steinberg::TUID cid, Steinberg::TUID _iid, void** obj) {
	*obj = nullptr;
	return Steinberg::kNotImplemented;
}

Steinberg::tresult PLUGIN_API VST3HostContext::queryInterface(const Steinberg::TUID _iid, void** obj) {
	QUERY_INTERFACE(_iid, obj, Steinberg::Vst::IHostApplication::iid, Steinberg::Vst::IHostApplication)
	*obj = nullptr;
	return Steinberg::kNoInterface;
}
Steinberg::uint32 PLUGIN_API VST3HostContext::addRef() {
	return ++mRefCount;
}
Steinberg::uint32 PLUGIN_API VST3HostContext::release() {
	return --mRefCount;
}

Steinberg::tresult PLUGIN_API VST3ComponentHandler::performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) {
	if (!mProcessor)
		return Steinberg::kResultFalse;
	const auto& params = mProcessor->GetParameters();
	if (id < params.size()) {
		params[id]->value = (float)valueNormalized;
	}
	return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API VST3ComponentHandler::queryInterface(const Steinberg::TUID _iid, void** obj) {
	QUERY_INTERFACE(_iid, obj, Steinberg::Vst::IComponentHandler::iid, Steinberg::Vst::IComponentHandler)
	*obj = nullptr;
	return Steinberg::kNoInterface;
}

Steinberg::uint32 PLUGIN_API VST3ComponentHandler::addRef() {
	return ++mRefCount;
}
Steinberg::uint32 PLUGIN_API VST3ComponentHandler::release() {
	if (--mRefCount == 0) {
		delete this;
		return 0;
	}
	return mRefCount;
}

VST3Processor::VST3Processor(const std::string& path, const std::string& classID)
	: mPath(path), mClassID(classID) {
	mEventList = std::make_unique<EventList>();
	mParamChanges = std::make_unique<ParameterChanges>();
}

VST3Processor::~VST3Processor() {
	CloseEditor();

	if (mIsActive && mComponent) {
		mComponent->setActive(false);
		mIsActive = false;
	}

	if (mController)
		mController->setComponentHandler(nullptr);

	if (mComponentHandler) {
		mComponentHandler->release();
		mComponentHandler = nullptr;
	}

	if (mController) {
		mController->terminate();
		mController->release();
	}
	if (mProcessor)
		mProcessor->release();
	if (mComponent) {
		mComponent->terminate();
		mComponent->release();
	}

#ifdef _WIN32
	if (mModuleHandle) {
		auto exitDll = (bool (*)())GetProcAddress((HMODULE)mModuleHandle, "ExitDll");
		if (exitDll)
			exitDll();
		// FreeLibrary((HMODULE)mModuleHandle);
	}
#endif
}

bool VST3Processor::Load() {
	if (mComponent)
		return true;

#ifdef _WIN32
	mModuleHandle = LoadLibraryA(mPath.c_str());
	if (!mModuleHandle) {
		std::cout << "Failed to load VST3 DLL: " << mPath << std::endl;
		return false;
	}

	auto initDll = (bool (*)())GetProcAddress((HMODULE)mModuleHandle, "InitDll");
	if (initDll)
		initDll();

	auto getFactory = (Steinberg::IPluginFactory * (*)()) GetProcAddress((HMODULE)mModuleHandle, "GetPluginFactory");
	if (!getFactory)
		return false;

	mFactory = getFactory();
	if (!mFactory)
		return false;

	Steinberg::int32 classCount = mFactory->countClasses();
	Steinberg::TUID targetUID;
	bool foundUID = false;

	for (Steinberg::int32 i = 0; i < classCount; ++i) {
		Steinberg::PClassInfo info;
		if (mFactory->getClassInfo(i, &info) == Steinberg::kResultTrue) {
			if (strcmp(info.category, "Audio Module Class") == 0) {
				char buf[33];
				for (int j = 0; j < 16; ++j)
					sprintf_s(buf + j * 2, 3, "%02X", (unsigned char)info.cid[j]);

				if (mClassID.empty() || mClassID == buf) {
					memcpy(targetUID, info.cid, sizeof(Steinberg::TUID));
					foundUID = true;
					if (mClassID.empty())
						mClassID = buf;

					mName = info.name; // properly extracts ascii name safely
					break;
				}
			}
		}
	}

	if (!foundUID) {
		std::cout << "No suitable VST3 class found in " << mPath << std::endl;
		return false;
	}

	if (mFactory->createInstance(targetUID, Steinberg::Vst::IComponent::iid, (void**)&mComponent) != Steinberg::kResultTrue) {
		std::cout << "Failed to create VST3 component." << std::endl;
		return false;
	}
#else
	return false;
#endif

	auto hostContext = VST3HostContext::GetInstance();
	if (mComponent->initialize(hostContext) != Steinberg::kResultTrue) {
		std::cout << "Failed to initialize VST3 component." << std::endl;
		return false;
	}

	if (mComponent->queryInterface(Steinberg::Vst::IAudioProcessor::iid, (void**)&mProcessor) != Steinberg::kResultTrue) {
		std::cout << "VST3 component does not implement IAudioProcessor." << std::endl;
		return false;
	}

	if (mComponent->queryInterface(Steinberg::Vst::IEditController::iid, (void**)&mController) != Steinberg::kResultTrue) {
		Steinberg::TUID controllerCID;
		if (mComponent->getControllerClassId(controllerCID) == Steinberg::kResultTrue) {
			mFactory->createInstance(controllerCID, Steinberg::Vst::IEditController::iid, (void**)&mController);
			if (mController) {
				mController->initialize(hostContext);
			}
		}
	}

	if (mController) {
		mComponentHandler = new VST3ComponentHandler(this);
		mController->setComponentHandler(mComponentHandler);

		MemoryStream stream;
		if (mComponent->getState(&stream) == Steinberg::kResultTrue) {
			stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
			mController->setComponentState(&stream);
		}
	}

	Steinberg::int32 eventInputs = mComponent->getBusCount(Steinberg::Vst::kEvent, Steinberg::Vst::kInput);
	if (eventInputs > 0) {
		mIsSynth = true;
	}

	if (!mIsSynth && mController) {
		Steinberg::int32 audioInputs = mComponent->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
		Steinberg::int32 audioOutputs = mComponent->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
		if (audioInputs == 0 && audioOutputs > 0)
			mIsSynth = true;
	}

	if (mName.empty()) {
		size_t lastSlash = mPath.find_last_of("\\/");
		mName = (lastSlash != std::string::npos) ? mPath.substr(lastSlash + 1) : mPath;
		size_t lastDot = mName.find_last_of('.');
		if (lastDot != std::string::npos)
			mName = mName.substr(0, lastDot);
	}

	InitializeParameters();
	return true;
}

void VST3Processor::InitializeParameters() {
	mParameters.clear();
	mLastSentValues.clear();

	if (!mController)
		return;

	Steinberg::int32 paramCount = mController->getParameterCount();
	for (Steinberg::int32 i = 0; i < paramCount; ++i) {
		Steinberg::Vst::ParameterInfo info = {};
		if (mController->getParameterInfo(i, info) != Steinberg::kResultTrue)
			continue;

		std::string nameStr;
		for (int j = 0; j < 128 && info.title[j] != 0; ++j) {
			nameStr += (char)info.title[j];
		}

		if (nameStr.empty()) {
			nameStr = "Param " + std::to_string(info.id);
		}

		float val = (float)mController->getParamNormalized(info.id);
		AddParameter(std::make_unique<SliderParameter>(nameStr, val, 0.0f, 1.0f));
		mLastSentValues.push_back(val);
	}
}

void VST3Processor::SetupBuses(int numChannels) {
	if (!mComponent)
		return;

	Steinberg::int32 audioInputs = mComponent->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
	for (Steinberg::int32 i = 0; i < audioInputs; ++i) {
		Steinberg::Vst::BusInfo busInfo = {};
		if (mComponent->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, i, busInfo) == Steinberg::kResultTrue) {
			mComponent->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, i, busInfo.channelCount > 0);
		}
	}

	Steinberg::int32 audioOutputs = mComponent->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
	for (Steinberg::int32 i = 0; i < audioOutputs; ++i) {
		Steinberg::Vst::BusInfo busInfo = {};
		if (mComponent->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, i, busInfo) == Steinberg::kResultTrue) {
			mComponent->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, i, busInfo.channelCount > 0);
		}
	}

	Steinberg::int32 eventInputs = mComponent->getBusCount(Steinberg::Vst::kEvent, Steinberg::Vst::kInput);
	for (Steinberg::int32 i = 0; i < eventInputs; ++i) {
		mComponent->activateBus(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, i, true);
	}
}

void VST3Processor::PrepareToPlay(double sampleRate) {
	if (!mProcessor || !mComponent)
		return;

	mSampleRate = sampleRate;

	Steinberg::Vst::ProcessSetup setup;
	setup.processMode = Steinberg::Vst::kRealtime;
	setup.symbolicSampleSize = Steinberg::Vst::kSample32;
	setup.maxSamplesPerBlock = 512;
	setup.sampleRate = sampleRate;

	mProcessor->setupProcessing(setup);

	Steinberg::int32 numInputs = mComponent->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
	Steinberg::int32 numOutputs = mComponent->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);

	if (numInputs > 0 || numOutputs > 0) {
		std::vector<Steinberg::Vst::SpeakerArrangement> inputs(numInputs, 3ULL);
		std::vector<Steinberg::Vst::SpeakerArrangement> outputs(numOutputs, 3ULL);
		mProcessor->setBusArrangements(inputs.data(), numInputs, outputs.data(), numOutputs);
	}

	SetupBuses(2);

	if (!mIsActive) {
		mComponent->setActive(true);
		mIsActive = true;
	}
}

void VST3Processor::Reset() {
	if (!mComponent)
		return;

	mNeedsFlush = true;
}

void VST3Processor::SyncParametersToController(int numFrames) {
	if (!mController)
		return;

	if (mLastSentValues.size() < mParameters.size()) {
		mLastSentValues.resize(mParameters.size(), -1.0f);
	}

	mParamChanges->clear();

	for (size_t i = 0; i < mParameters.size(); ++i) {
		float hostVal = mParameters[i]->value;
		float lastSent = mLastSentValues[i];

		if (std::abs(hostVal - lastSent) > 0.000001f) {
			Steinberg::Vst::ParamID id = (Steinberg::Vst::ParamID)i;
			Steinberg::int32 index = 0;
			auto queue = mParamChanges->addParameterData(id, index);
			if (queue) {
				queue->addPoint(0, hostVal, index);
			}
			mController->setParamNormalized(id, hostVal);
			mLastSentValues[i] = hostVal;
		}
	}
}

void VST3Processor::ConvertMIDIToEvents(std::vector<MIDIMessage>& midiMessages) {
	mEventList->clear();

	for (const auto& msg : midiMessages) {
		uint8_t status = msg.status;
		uint8_t type = status & 0xF0;
		uint8_t channel = status & 0x0F;

		Steinberg::Vst::Event e = {};
		e.sampleOffset = msg.frameIndex;

		if (type == 0x90) {
			if (msg.data2 > 0) {
				e.type = Steinberg::Vst::Event::kNoteOnEvent;
				e.noteOn.channel = channel;
				e.noteOn.pitch = msg.data1;
				e.noteOn.velocity = msg.data2 / 127.0f;
				e.noteOn.length = 0;
				e.noteOn.tuning = 0.0f;
				e.noteOn.noteId = -1;
				mEventList->addEvent(e);
				mActiveMIDINotes[channel].insert(msg.data1);
			} else {
				e.type = Steinberg::Vst::Event::kNoteOffEvent;
				e.noteOff.channel = channel;
				e.noteOff.pitch = msg.data1;
				e.noteOff.velocity = 0.0f;
				e.noteOff.noteId = -1;
				mEventList->addEvent(e);
				mActiveMIDINotes[channel].erase(msg.data1);
			}
		} else if (type == 0x80) {
			e.type = Steinberg::Vst::Event::kNoteOffEvent;
			e.noteOff.channel = channel;
			e.noteOff.pitch = msg.data1;
			e.noteOff.velocity = msg.data2 / 127.0f;
			e.noteOff.noteId = -1;
			mEventList->addEvent(e);
			mActiveMIDINotes[channel].erase(msg.data1);
		} else if (type == 0xB0) {
			e.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
			e.midiCCOut.channel = channel;
			e.midiCCOut.controlNumber = msg.data1;
			e.midiCCOut.value = msg.data2;
			mEventList->addEvent(e);
		}
	}
}

void VST3Processor::Process(float* buffer, int numFrames, int numChannels,
							std::vector<MIDIMessage>& mIDIMessages,
							const ProcessContext& context) {
	if (!mProcessor || !mIsActive)
		return;

	SyncParametersToController(numFrames);

	if (mNeedsFlush) {
		for (int ch = 0; ch < 16; ++ch) {
			for (int note : mActiveMIDINotes[ch]) {
				MIDIMessage msg;
				msg.status = 0x80 | ch;
				msg.data1 = note;
				msg.data2 = 0;
				msg.frameIndex = 0;
				mIDIMessages.push_back(msg);
			}
			mActiveMIDINotes[ch].clear();

			MIDIMessage msgCC;
			msgCC.status = 0xB0 | ch;
			msgCC.data1 = 123;
			msgCC.data2 = 0;
			msgCC.frameIndex = 0;
			mIDIMessages.push_back(msgCC);
		}
		mNeedsFlush = false;
	}

	Steinberg::int32 numInputs = mComponent->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
	Steinberg::int32 numOutputs = mComponent->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);

	int totalInChannels = 0;
	for (Steinberg::int32 b = 0; b < numInputs; ++b) {
		Steinberg::Vst::BusInfo busInfo = {};
		if (mComponent->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, b, busInfo) == Steinberg::kResultTrue) {
			totalInChannels += busInfo.channelCount;
		}
	}

	int totalOutChannels = 0;
	for (Steinberg::int32 b = 0; b < numOutputs; ++b) {
		Steinberg::Vst::BusInfo busInfo = {};
		if (mComponent->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, b, busInfo) == Steinberg::kResultTrue) {
			totalOutChannels += busInfo.channelCount;
		}
	}

	int maxChannels = (std::max)(totalInChannels, totalOutChannels);
	if (maxChannels == 0)
		maxChannels = 2;

	int requiredBufferSize = numFrames * maxChannels * 2;
	if ((int)mProcessBuffer.size() < requiredBufferSize) {
		mProcessBuffer.resize(requiredBufferSize);
	}

	if ((int)mInputPtrs.size() < totalInChannels)
		mInputPtrs.resize(totalInChannels);
	if ((int)mOutputPtrs.size() < totalOutChannels)
		mOutputPtrs.resize(totalOutChannels);

	totalInChannels = 0;
	for (Steinberg::int32 b = 0; b < numInputs; ++b) {
		Steinberg::Vst::BusInfo busInfo = {};
		if (mComponent->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, b, busInfo) == Steinberg::kResultTrue) {
			for (int c = 0; c < busInfo.channelCount; ++c) {
				mInputPtrs[totalInChannels] = &mProcessBuffer[totalInChannels * numFrames];
				if (totalInChannels < numChannels) {
					for (int i = 0; i < numFrames; ++i) {
						mInputPtrs[totalInChannels][i] = buffer[i * numChannels + totalInChannels];
					}
				} else {
					std::fill(mInputPtrs[totalInChannels], mInputPtrs[totalInChannels] + numFrames, 0.0f);
				}
				totalInChannels++;
			}
		}
	}

	totalOutChannels = 0;
	int outOffset = totalInChannels * numFrames;
	for (Steinberg::int32 b = 0; b < numOutputs; ++b) {
		Steinberg::Vst::BusInfo busInfo = {};
		if (mComponent->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, b, busInfo) == Steinberg::kResultTrue) {
			for (int c = 0; c < busInfo.channelCount; ++c) {
				mOutputPtrs[totalOutChannels] = &mProcessBuffer[outOffset + totalOutChannels * numFrames];
				totalOutChannels++;
			}
		}
	}

	ConvertMIDIToEvents(mIDIMessages);

	std::vector<Steinberg::Vst::AudioBusBuffers> inBuses;
	std::vector<Steinberg::Vst::AudioBusBuffers> outBuses;

	if (numInputs > 0) {
		inBuses.resize(numInputs);
		int chIdx = 0;
		for (Steinberg::int32 b = 0; b < numInputs; ++b) {
			Steinberg::Vst::BusInfo busInfo = {};
			Steinberg::int32 busChans = 0;
			if (mComponent->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, b, busInfo) == Steinberg::kResultTrue)
				busChans = busInfo.channelCount;
			inBuses[b].numChannels = busChans;
			inBuses[b].channelBuffers32 = busChans > 0 ? &mInputPtrs[chIdx] : nullptr;
			chIdx += busChans;
		}
	}
	if (numOutputs > 0) {
		outBuses.resize(numOutputs);
		int chIdx = 0;
		for (Steinberg::int32 b = 0; b < numOutputs; ++b) {
			Steinberg::Vst::BusInfo busInfo = {};
			Steinberg::int32 busChans = 0;
			if (mComponent->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, b, busInfo) == Steinberg::kResultTrue)
				busChans = busInfo.channelCount;
			outBuses[b].numChannels = busChans;
			outBuses[b].channelBuffers32 = busChans > 0 ? &mOutputPtrs[chIdx] : nullptr;
			chIdx += busChans;
		}
	}

	Steinberg::Vst::ProcessData data = {};
	data.processMode = Steinberg::Vst::kRealtime;
	data.symbolicSampleSize = Steinberg::Vst::kSample32;
	data.numSamples = numFrames;
	data.numInputs = numInputs;
	data.numOutputs = numOutputs;
	data.inputs = inBuses.empty() ? nullptr : inBuses.data();
	data.outputs = outBuses.empty() ? nullptr : outBuses.data();
	data.inputEvents = mEventList->getEventCount() > 0 ? mEventList.get() : nullptr;
	data.outputEvents = nullptr;
	data.inputParameterChanges = mParamChanges->getParameterCount() > 0 ? mParamChanges.get() : nullptr;
	data.outputParameterChanges = nullptr;

	Steinberg::Vst::ProcessContext vstContext = {};
	vstContext.state = 0;
	if (context.isPlaying)
		vstContext.state |= Steinberg::Vst::ProcessContext::kPlaying;

	vstContext.state |= Steinberg::Vst::ProcessContext::kTempoValid;
	vstContext.state |= Steinberg::Vst::ProcessContext::kTimeSigValid;
	vstContext.state |= Steinberg::Vst::ProcessContext::kProjectTimeMusicValid;

	vstContext.sampleRate = context.sampleRate;
	vstContext.projectTimeSamples = context.currentSample;
	vstContext.tempo = context.bpm;
	vstContext.timeSigNumerator = (Steinberg::int32)context.timeSigNumerator;
	vstContext.timeSigDenominator = (Steinberg::int32)context.timeSigDenominator;

	double samplesPerBeat = (context.sampleRate * 60.0) / context.bpm;
	vstContext.projectTimeMusic = context.currentSample / samplesPerBeat;

	data.processContext = &vstContext;

	mProcessor->process(data);

	int actualOutChans = totalOutChannels;
	for (int i = 0; i < numFrames; ++i) {
		for (int c = 0; c < numChannels; ++c) {
			float val = 0.0f;
			if (c < actualOutChans) {
				val = mOutputPtrs[c][i];
			} else if (c == 1 && actualOutChans == 1) {
				val = mOutputPtrs[0][i];
			}
			buffer[i * numChannels + c] = val;
		}
	}

	if (data.outputParameterChanges && mController) {
		Steinberg::int32 paramCount = data.outputParameterChanges->getParameterCount();
		for (Steinberg::int32 i = 0; i < paramCount; ++i) {
			auto queue = data.outputParameterChanges->getParameterData(i);
			if (queue) {
				Steinberg::int32 pointCount = queue->getPointCount();
				if (pointCount > 0) {
					Steinberg::int32 sampleOffset;
					Steinberg::Vst::ParamValue value;
					queue->getPoint(pointCount - 1, sampleOffset, value);
					Steinberg::Vst::ParamID id = queue->getParameterId();
					if (id < mParameters.size()) {
						mParameters[id]->value = (float)value;
						if (id < mLastSentValues.size())
							mLastSentValues[id] = (float)value;
					}
				}
			}
		}
	}
}

void VST3Processor::Save(std::ostream& out) {
	out << "VST3_BEGIN\n";
	out << "PATH \"" << mPath << "\"\n";
	out << "CLASSID \"" << mClassID << "\"\n";

	if (mComponent) {
		MemoryStream stream;
		if (mComponent->getState(&stream) == Steinberg::kResultTrue) {
			Steinberg::int64 size;
			stream.tell(&size);
			stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
			std::vector<unsigned char> data((size_t)size);
			Steinberg::int32 read = 0;
			stream.read(data.data(), (Steinberg::int32)size, &read);
			if (read > 0) {
				std::string encoded = Base64Encode(data.data(), read);
				out << "CHUNK " << encoded << "\n";
			}
		}
	}

	if (mController) {
		MemoryStream stream;
		if (mController->getState(&stream) == Steinberg::kResultTrue) {
			Steinberg::int64 size;
			stream.tell(&size);
			stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
			std::vector<unsigned char> data((size_t)size);
			Steinberg::int32 read = 0;
			stream.read(data.data(), (Steinberg::int32)size, &read);
			if (read > 0) {
				std::string encoded = Base64Encode(data.data(), read);
				out << "CTRL_CHUNK " << encoded << "\n";
			}
		}
	}

	out << "PARAMS_BEGIN\n";
	for (int i = 0; i < (int)mParameters.size(); ++i) {
		out << "P " << i << " " << mParameters[i]->value << "\n";
	}
	out << "PARAMS_END\n";

	out << "VST3_END\n";
}

void VST3Processor::Load(std::istream& in) {
	std::string line;
	bool loadedChunk = false;
	while (std::getline(in, line)) {
		if (line == "VST3_END")
			break;

		if (line.rfind("PATH ", 0) == 0) {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos) {
				mPath = line.substr(q1 + 1, q2 - q1 - 1);
			}
		} else if (line.rfind("CLASSID ", 0) == 0) {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos) {
				mClassID = line.substr(q1 + 1, q2 - q1 - 1);
				if (!mPath.empty()) {
					if (!Load()) {
						std::cout << "Failed to load VST3 from serialized path: " << mPath << "\n";
					}
				}
			}
		} else if (line.rfind("CHUNK ", 0) == 0) {
			std::string b64 = line.substr(6);
			std::vector<unsigned char> data = Base64Decode(b64);
			if (!data.empty() && mComponent) {
				MemoryStream stream(data.data(), (Steinberg::int32)data.size());
				mComponent->setState(&stream);
				if (mController) {
					stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
					mController->setComponentState(&stream);
				}
				loadedChunk = true;
			}
		} else if (line.rfind("CTRL_CHUNK ", 0) == 0) {
			std::string b64 = line.substr(11);
			std::vector<unsigned char> data = Base64Decode(b64);
			if (!data.empty() && mController) {
				MemoryStream stream(data.data(), (Steinberg::int32)data.size());
				mController->setState(&stream);
			}
		} else if (line.rfind("P ", 0) == 0) {
			int idx;
			float val;
			if (sscanf_s(line.c_str(), "P %d %f", &idx, &val) == 2) {
				if (mController && idx < (int)mParameters.size()) {
					if (!loadedChunk) {
						Steinberg::Vst::ParamID id = (Steinberg::Vst::ParamID)idx;
						mController->setParamNormalized(id, val);
						mParameters[idx]->value = val;
						if (idx < (int)mLastSentValues.size())
							mLastSentValues[idx] = val;
					}
				}
			}
		}
	}

	if (!mComponent && !mPath.empty()) {
		if (!Load()) {
			std::cout << "Failed to load VST3 from serialized path: " << mPath << "\n";
		}
	} else if (loadedChunk) {
		InitializeParameters(); // refetch linked parameters from macro overrides
	}
}

#ifdef _WIN32
class VST3PlugFrame : public Steinberg::IPlugFrame {
public:
	VST3PlugFrame(HWND hwnd) : mHwnd(hwnd) {}
	virtual ~VST3PlugFrame() = default;

	Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override {
		if (mHwnd && newSize) {
			// newSize is in physical pixels; size the non-client area for this
			// window's DPI so the client area matches the requested view exactly
			RECT wr = {0, 0, newSize->getWidth(), newSize->getHeight()};
			DWORD style = (DWORD)GetWindowLongPtrW(mHwnd, GWL_STYLE);
			AdjustWindowRectExForDpi(&wr, style, FALSE, 0, GetDpiForWindow(mHwnd));
			SetWindowPos(mHwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			return Steinberg::kResultTrue;
		}
		return Steinberg::kResultFalse;
	}

	DECLARE_FUNKNOWN_METHODS
private:
	Steinberg::uint32 mRefCount = 1;
	HWND mHwnd;
};

Steinberg::tresult PLUGIN_API VST3PlugFrame::queryInterface(const Steinberg::TUID _iid, void** obj) {
	QUERY_INTERFACE(_iid, obj, Steinberg::IPlugFrame::iid, Steinberg::IPlugFrame)
	*obj = nullptr;
	return Steinberg::kNoInterface;
}
Steinberg::uint32 PLUGIN_API VST3PlugFrame::addRef() {
	return ++mRefCount;
}
Steinberg::uint32 PLUGIN_API VST3PlugFrame::release() {
	if (--mRefCount == 0) {
		delete this;
		return 0;
	}
	return mRefCount;
}

LRESULT CALLBACK VST3Processor::EditorWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	VST3Processor* proc = (VST3Processor*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg) {
	case WM_CLOSE:
		if (proc)
			proc->CloseEditor();
		return 0;
	case WM_SIZE:
		// complete the resize handshake: whenever the window changes size (e.g.
		// the plugin requested it via IPlugFrame::resizeView after an internal
		// scale change), tell the view its new client size so it re-lays out its
		// content. Without this the plugin only picks up the new size on reopen
		if (proc && proc->mPlugView && wParam != SIZE_MINIMIZED) {
			RECT rc;
			GetClientRect(hwnd, &rc);
			Steinberg::ViewRect vr(0, 0, rc.right, rc.bottom);
			proc->mPlugView->onSize(&vr);
		}
		break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool VST3Processor::HasEditor() const {
	if (!mController)
		return false;
	Steinberg::IPlugView* view = mController->createView(Steinberg::Vst::ViewType::kEditor);
	if (view) {
		view->release();
		return true;
	}
	return false;
}

void VST3Processor::OpenEditor(void* parentWindowHandle) {
	if (!mController || mEditorWindow)
		return;

	HWND hParent = (HWND)parentWindowHandle;

	// Native mode: create the window DPI-aware and tell the plugin the display
	// scale via IPlugViewContentScaleSupport, so modern VST3s render their UI
	// crisply at the real resolution (and report a correctly-scaled size). Scaled
	// mode keeps the legacy DPI-unaware path where Windows stretches the window
	bool native = UseNativeEditorScaling();
	DPI_AWARENESS_CONTEXT wanted = native ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
										  : DPI_AWARENESS_CONTEXT_UNAWARE;
	DPI_AWARENESS_CONTEXT oldContext = SetThreadDpiAwarenessContext(wanted);

	UINT dpi = (native && hParent) ? GetDpiForWindow(hParent) : 96;
	float scaleFactor = (float)dpi / 96.0f;

	mPlugView = mController->createView(Steinberg::Vst::ViewType::kEditor);
	if (!mPlugView) {
		SetThreadDpiAwarenessContext(oldContext);
		return;
	}

	// negotiate content scale BEFORE getSize so the plugin reports physical pixels
	if (native) {
		Steinberg::IPlugViewContentScaleSupport* scaleSupport = nullptr;
		if (mPlugView->queryInterface(Steinberg::IPlugViewContentScaleSupport::iid, (void**)&scaleSupport) == Steinberg::kResultTrue && scaleSupport) {
			scaleSupport->setContentScaleFactor((Steinberg::IPlugViewContentScaleSupport::ScaleFactor)scaleFactor);
			scaleSupport->release();
		}
	}

	Steinberg::ViewRect rect = {};
	mPlugView->getSize(&rect);
	int width = rect.getWidth();
	int height = rect.getHeight();
	if (width <= 0)
		width = 400;
	if (height <= 0)
		height = 300;

	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.lpfnWndProc = EditorWindowProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"VST3EditorClass";
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	RegisterClassExW(&wc);

	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
	RECT wr = {0, 0, width, height};
	AdjustWindowRectExForDpi(&wr, style, FALSE, 0, dpi);

	std::wstring wName;
	if (!mName.empty()) {
		int len = MultiByteToWideChar(CP_UTF8, 0, mName.c_str(), (int)mName.length(), NULL, 0);
		if (len > 0) {
			wName.resize(len);
			MultiByteToWideChar(CP_UTF8, 0, mName.c_str(), (int)mName.length(), &wName[0], len);
		}
	}
	std::wstring wTitle = L"Editor: " + wName;

	mEditorWindow = CreateWindowExW(0, L"VST3EditorClass", wTitle.c_str(),
									style, CW_USEDEFAULT, CW_USEDEFAULT,
									wr.right - wr.left, wr.bottom - wr.top,
									hParent, NULL, GetModuleHandleW(NULL), NULL);

	SetWindowLongPtrW(mEditorWindow, GWLP_USERDATA, (LONG_PTR)this);

	mPlugFrame = new VST3PlugFrame(mEditorWindow);
	mPlugView->setFrame(mPlugFrame);

	if (mPlugView->attached(mEditorWindow, Steinberg::kPlatformTypeHWND) != Steinberg::kResultTrue) {
		DestroyWindow(mEditorWindow);
		mEditorWindow = nullptr;
		mPlugView->setFrame(nullptr);
		mPlugFrame->release();
		mPlugFrame = nullptr;
		mPlugView->release();
		mPlugView = nullptr;
		SetThreadDpiAwarenessContext(oldContext);
		return;
	}

	mPlugView->onWheel(0.0f);
	ShowWindow(mEditorWindow, SW_SHOW);
	SetForegroundWindow(mEditorWindow);
	SetFocus(mEditorWindow);
	UpdateWindow(mEditorWindow);
	SetThreadDpiAwarenessContext(oldContext);
}

void VST3Processor::CloseEditor() {
	if (mPlugView) {
		mPlugView->setFrame(nullptr);
		mPlugView->removed();
		mPlugView->release();
		mPlugView = nullptr;
	}
	if (mPlugFrame) {
		mPlugFrame->release();
		mPlugFrame = nullptr;
	}
	if (mEditorWindow) {
		// Hide before destroy and hand activation back to the owner explicitly.
		// Destroying a visible foreground owned window otherwise lets Windows pick
		// the next window to activate, which can minimize the main DAW window
		HWND owner = GetWindow(mEditorWindow, GW_OWNER);
		ShowWindow(mEditorWindow, SW_HIDE);
		DestroyWindow(mEditorWindow);
		mEditorWindow = nullptr;
		if (owner)
			SetActiveWindow(owner);
	}
}

bool VST3Processor::IsEditorOpen() const {
	return mEditorWindow != nullptr;
}

void VST3Processor::EditorIdle() {
	// Some plugins (e.g. Serum, OTT) change their editor size from their own UI
	// without driving the host resize handshake. When they do, they resize the
	// child HWND they created inside our editor window. Mirror our window's client
	// area to that child each frame so the window tracks the plugin size live,
	// regardless of whether the plugin also calls resizeView()/updates getSize()
	if (!mEditorWindow)
		return;

	HWND child = GetWindow(mEditorWindow, GW_CHILD);
	if (!child)
		return;

	RECT childRc;
	GetClientRect(child, &childRc);
	int childW = childRc.right - childRc.left;
	int childH = childRc.bottom - childRc.top;
	if (childW <= 0 || childH <= 0)
		return;

	RECT clientRc;
	GetClientRect(mEditorWindow, &clientRc);
	int clientW = clientRc.right - clientRc.left;
	int clientH = clientRc.bottom - clientRc.top;

	// tolerance avoids a resize feedback loop from sub-pixel non-client rounding
	if (std::abs(childW - clientW) > 1 || std::abs(childH - clientH) > 1) {
		RECT wr = {0, 0, childW, childH};
		DWORD style = (DWORD)GetWindowLongPtrW(mEditorWindow, GWL_STYLE);
		AdjustWindowRectExForDpi(&wr, style, FALSE, 0, GetDpiForWindow(mEditorWindow));
		SetWindowPos(mEditorWindow, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
					 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

#else
bool VST3Processor::HasEditor() const {
	return false;
}
void VST3Processor::OpenEditor(void* parentWindowHandle) {
	(void)parentWindowHandle;
}
void VST3Processor::CloseEditor() {}
bool VST3Processor::IsEditorOpen() const {
	return false;
}
#endif

#ifdef _WIN32
std::vector<PluginInfo> VST3Processor::EnumeratePlugins(const std::string& path) {
	std::vector<PluginInfo> results;
	HMODULE moduleHandle = LoadLibraryA(path.c_str());
	if (!moduleHandle)
		return results;

	auto initDll = (bool (*)())GetProcAddress(moduleHandle, "InitDll");
	if (initDll)
		initDll();

	auto getFactory = (Steinberg::IPluginFactory * (*)()) GetProcAddress(moduleHandle, "GetPluginFactory");
	if (!getFactory) {
		// intentional lack of FreeLibrary (attempt to avoid un-loading crashes)
		return results;
	}

	Steinberg::IPluginFactory* factory = getFactory();
	if (!factory) {
		return results;
	}

	Steinberg::int32 classCount = factory->countClasses();
	for (Steinberg::int32 i = 0; i < classCount; ++i) {
		Steinberg::PClassInfo info;
		if (factory->getClassInfo(i, &info) == Steinberg::kResultTrue) {
			if (strcmp(info.category, "Audio Module Class") == 0) {
				char buf[33];
				for (int j = 0; j < 16; ++j)
					sprintf_s(buf + j * 2, 3, "%02X", (unsigned char)info.cid[j]);

				PluginInfo pinfo;
				pinfo.name = info.name;
				pinfo.path = path;
				pinfo.classID = buf;
				pinfo.format = "VST3";

				Steinberg::IPluginFactory3* factory3 = nullptr;
				if (factory->queryInterface(Steinberg::IPluginFactory3::iid, (void**)&factory3) == Steinberg::kResultTrue) {
					Steinberg::PClassInfoW infoW;
					if (factory3->getClassInfoUnicode(i, &infoW) == Steinberg::kResultTrue) {
						std::string subCat;
						for (int k = 0; k < 128 && infoW.subCategories[k] != 0; ++k) {
							subCat += (char)infoW.subCategories[k];
						}
						if (subCat.find("Instrument") != std::string::npos) {
							pinfo.isSynth = true;
						} else {
							pinfo.isSynth = false;
						}
					}
					factory3->release();
				} else {
					pinfo.isSynth = false;
				}

				results.push_back(pinfo);
			}
		}
	}

	auto exitDll = (bool (*)())GetProcAddress(moduleHandle, "ExitDll");
	if (exitDll)
		exitDll();

	// intentional lack of FreeLibrary (attempt to avoid un-loading crashes)
	return results;
}
#else
std::vector<PluginInfo> VST3Processor::EnumeratePlugins(const std::string& path) {
	return {};
}
#endif
