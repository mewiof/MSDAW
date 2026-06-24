#include "Parameters/SliderParameter.h"
#include "PrecompHeader.h"
#include "VSTProcessor.h"
#include <iostream>
#include <mutex>
#include <map>
#include <algorithm>
#include <cstring>
#include <vector>
#include <cmath>

// base64 helper for chunk serialization
static const std::string base64_chars =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789+/";

static std::string Base64Encode(const unsigned char* bytes_to_encode, unsigned int in_len) {
	std::string ret;
	int i = 0;
	int j = 0;
	unsigned char char_array_3[3];
	unsigned char char_array_4[4];

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
	int i = 0;
	int j = 0;
	int in_ = 0;
	unsigned char char_array_4[4], char_array_3[3];
	std::vector<unsigned char> ret;

	auto is_base64 = [](unsigned char c) {
		return (isalnum(c) || (c == '+') || (c == '/'));
	};

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

// global map to route static callbacks to instances
static std::mutex gEffectMapMutex;
static std::map<AEffect*, VSTProcessor*> gEffectMap;

// initialize the static callback
std::function<void(int, bool)> VSTProcessor::OnGlobalKeyEvent = nullptr;

// dispatcher signature
typedef AEffect*(VSTCALLBACK* VSTPluginMainProc)(audioMasterCallback audioMaster);

VSTProcessor::VSTProcessor(const std::string& path)
	: mPath(path) {
	memset(&mTimeInfo, 0, sizeof(VstTimeInfo));
	mTimeInfo.sampleRate = 48000.0;
	mTimeInfo.tempo = 120.0;
	mTimeInfo.timeSigNumerator = 4;
	mTimeInfo.timeSigDenominator = 4;
}

VSTProcessor::~VSTProcessor() {
	CloseEditor();
	Suspend();

	if (mAEffect) {
		{
			std::lock_guard<std::mutex> lock(gEffectMapMutex);
			gEffectMap.erase(mAEffect);
		}
		mAEffect->dispatcher(mAEffect, effClose, 0, 0, 0, 0.0f);
		mAEffect = nullptr;
	}

#ifdef _WIN32
	if (mLibrary) {
		// FreeLibrary(mLibrary);
		mLibrary = nullptr;
	}
#endif
}

bool VSTProcessor::Load() {
	if (mAEffect)
		return true;

#ifdef _WIN32
	mLibrary = LoadLibraryA(mPath.c_str());
	if (!mLibrary) {
		std::cout << "Failed to load VST DLL: " << mPath << " Error: " << GetLastError() << std::endl;
		return false;
	}

	VSTPluginMainProc mainProc = (VSTPluginMainProc)GetProcAddress(mLibrary, "VSTPluginMain");
	if (!mainProc)
		mainProc = (VSTPluginMainProc)GetProcAddress(mLibrary, "main");

	if (!mainProc) {
		std::cout << "VST Entry point not found in " << mPath << std::endl;
		return false;
	}

	mAEffect = mainProc(&VSTProcessor::HostCallback);
	if (!mAEffect || mAEffect->magic != kEffectMagic) {
		std::cout << "Invalid VST magic number." << std::endl;
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(gEffectMapMutex);
		gEffectMap[mAEffect] = this;
	}

	mAEffect->dispatcher(mAEffect, effOpen, 0, 0, 0, 0.0f);

	// get name
	char nameBuf[64] = {0};
	mAEffect->dispatcher(mAEffect, effGetEffectName, 0, 0, nameBuf, 0.0f);
	if (nameBuf[0])
		mName = nameBuf;
	else
		mName = "VST Plugin";

	// determine if this is an instrument
	if (mAEffect->flags & effFlagsIsSynth) {
		mIsSynth = true;
	} else {
		VstIntPtr category = mAEffect->dispatcher(mAEffect, effGetPlugCategory, 0, 0, 0, 0.0f);
		if (category == kPlugCategSynth) {
			mIsSynth = true;
		}
	}

	InitializeParameters();

	return true;
#else
	return false;
#endif
}

void VSTProcessor::InitializeParameters() {
	mParameters.clear();
	mLastSentValues.clear();

	for (int i = 0; i < mAEffect->numParams; ++i) {
		char paramName[64] = {0};
		char paramDisplay[64] = {0};
		char paramLabel[64] = {0};

		mAEffect->dispatcher(mAEffect, effGetParamName, i, 0, paramName, 0.0f);
		mAEffect->dispatcher(mAEffect, effGetParamDisplay, i, 0, paramDisplay, 0.0f);
		mAEffect->dispatcher(mAEffect, effGetParamLabel, i, 0, paramLabel, 0.0f);

		std::string name = paramName;
		if (name.empty())
			name = "Param " + std::to_string(i);

		float val = mAEffect->getParameter(mAEffect, i);
		AddParameter(std::make_unique<SliderParameter>(name, val, 0.0f, 1.0f));
		mLastSentValues.push_back(val);
	}
}

void VSTProcessor::PrepareToPlay(double sampleRate) {
	if (!mAEffect)
		return;

	mAEffect->dispatcher(mAEffect, effSetSampleRate, 0, 0, 0, (float)sampleRate);
	mAEffect->dispatcher(mAEffect, effSetBlockSize, 0, 512, 0, 0.0f);

	Resume();
}

void VSTProcessor::Resume() {
	if (mAEffect)
		mAEffect->dispatcher(mAEffect, effMainsChanged, 0, 1, 0, 0.0f);
}

void VSTProcessor::Suspend() {
	if (mAEffect)
		mAEffect->dispatcher(mAEffect, effMainsChanged, 0, 0, 0, 0.0f);
}

void VSTProcessor::Reset() {
	if (!mAEffect)
		return;

	// calculate space needed for panic ccs + note offs for stuck notes
	const int kNumChannels = 16;
	const int kPanicEventsPerChannel = 2; // cc 123, cc 120
	size_t activeNoteCount = 0;
	for (int i = 0; i < kNumChannels; ++i) {
		activeNoteCount += mActiveMIDINotes[i].size();
	}

	size_t totalEvents = (kNumChannels * kPanicEventsPerChannel) + activeNoteCount;

	const size_t kEventsOffset = (size_t)((char*)&((VstEvents*)nullptr)->events - (char*)nullptr);
	size_t requiredBufferSize = kEventsOffset + (totalEvents * sizeof(VstEvent*));

	if (mVSTEventBuffer.size() < requiredBufferSize) {
		mVSTEventBuffer.resize(requiredBufferSize);
	}

	VstEvents* vSTEvents = (VstEvents*)mVSTEventBuffer.data();
	vSTEvents->numEvents = (VstInt32)totalEvents;
	vSTEvents->reserved = 0;

	size_t ptrArraySize = totalEvents * sizeof(VstEvent*);
	size_t dataSize = totalEvents * sizeof(VstMidiEvent);
	size_t totalBufferSize = kEventsOffset + ptrArraySize + dataSize;

	if (mVSTEventBuffer.size() < totalBufferSize)
		mVSTEventBuffer.resize(totalBufferSize);

	vSTEvents = (VstEvents*)mVSTEventBuffer.data();
	uint8_t* eventDataStart = mVSTEventBuffer.data() + kEventsOffset + ptrArraySize;

	int eventIdx = 0;

	// 1. send explicit note offs for all active notes
	for (int ch = 0; ch < kNumChannels; ++ch) {
		for (int note : mActiveMIDINotes[ch]) {
			VstMidiEvent* e = (VstMidiEvent*)(eventDataStart + eventIdx * sizeof(VstMidiEvent));
			memset(e, 0, sizeof(VstMidiEvent));
			e->type = kVstMidiType;
			e->byteSize = sizeof(VstMidiEvent);
			e->deltaFrames = 0;
			e->midiData[0] = (char)(0x80 | ch);
			e->midiData[1] = (char)note;
			e->midiData[2] = 0;

			vSTEvents->events[eventIdx] = (VstEvent*)e;
			eventIdx++;
		}
		mActiveMIDINotes[ch].clear();
	}

	// 2. send panic ccs
	for (int ch = 0; ch < kNumChannels; ++ch) {
		// cc 123 (all notes off)
		{
			VstMidiEvent* e = (VstMidiEvent*)(eventDataStart + eventIdx * sizeof(VstMidiEvent));
			memset(e, 0, sizeof(VstMidiEvent));
			e->type = kVstMidiType;
			e->byteSize = sizeof(VstMidiEvent);
			e->deltaFrames = 0;
			e->midiData[0] = (char)(0xB0 | ch);
			e->midiData[1] = 123;
			e->midiData[2] = 0;
			vSTEvents->events[eventIdx] = (VstEvent*)e;
			eventIdx++;
		}
		// cc 120 (all sound off)
		{
			VstMidiEvent* e = (VstMidiEvent*)(eventDataStart + eventIdx * sizeof(VstMidiEvent));
			memset(e, 0, sizeof(VstMidiEvent));
			e->type = kVstMidiType;
			e->byteSize = sizeof(VstMidiEvent);
			e->deltaFrames = 0;
			e->midiData[0] = (char)(0xB0 | ch);
			e->midiData[1] = 120;
			e->midiData[2] = 0;
			vSTEvents->events[eventIdx] = (VstEvent*)e;
			eventIdx++;
		}
	}

	mAEffect->dispatcher(mAEffect, effProcessEvents, 0, 0, vSTEvents, 0.0f);
}

void VSTProcessor::Process(float* buffer, int numFrames, int numChannels,
						   std::vector<MIDIMessage>& mIDIMessages,
						   const ProcessContext& context) {
	if (!mAEffect)
		return;

	mTimeInfo.samplePos = context.currentSample;
	mTimeInfo.sampleRate = context.sampleRate;
	mTimeInfo.nanoSeconds = 0;
	double seconds = context.currentSample / context.sampleRate;
	mTimeInfo.ppqPos = seconds * (context.bpm / 60.0);
	mTimeInfo.tempo = context.bpm;
	mTimeInfo.barStartPos = std::floor(mTimeInfo.ppqPos / context.timeSigNumerator) * context.timeSigNumerator;
	mTimeInfo.cycleStartPos = 0;
	mTimeInfo.cycleEndPos = 0;
	mTimeInfo.timeSigNumerator = (VstInt32)context.timeSigNumerator;
	mTimeInfo.timeSigDenominator = (VstInt32)context.timeSigDenominator;
	mTimeInfo.smpteOffset = 0;
	mTimeInfo.smpteFrameRate = 0;
	mTimeInfo.samplesToNextClock = 0;

	mTimeInfo.flags = 0;
	mTimeInfo.flags |= kVstTempoValid | kVstTimeSigValid | kVstPpqPosValid | kVstBarsValid;
	if (context.isPlaying)
		mTimeInfo.flags |= kVstTransportPlaying;

	// 1. sync parameters (bi-directional logic)
	if (mLastSentValues.size() < mParameters.size()) {
		mLastSentValues.resize(mParameters.size(), -1.0f);
	}

	for (int i = 0; i < mAEffect->numParams && i < (int)mParameters.size(); ++i) {
		float hostVal = mParameters[i]->value;
		float lastSent = mLastSentValues[i];

		if (std::abs(hostVal - lastSent) > 0.000001f) {
			mAEffect->setParameter(mAEffect, i, hostVal);
			mLastSentValues[i] = hostVal;
		}
		// note: if hostval == lastsent, we do nothing
	}

	// 2. prepare buffers
	int maxChannels = max(mAEffect->numInputs, mAEffect->numOutputs);
	int requiredBufferSize = numFrames * maxChannels * 2;
	if ((int)mProcessBuffer.size() < requiredBufferSize) {
		mProcessBuffer.resize(requiredBufferSize);
	}

	if (mInputPtrs.size() != (size_t)mAEffect->numInputs)
		mInputPtrs.resize(mAEffect->numInputs);
	if (mOutputPtrs.size() != (size_t)mAEffect->numOutputs)
		mOutputPtrs.resize(mAEffect->numOutputs);

	for (int c = 0; c < mAEffect->numInputs; ++c) {
		mInputPtrs[c] = &mProcessBuffer[c * numFrames];
		if (c < numChannels) {
			for (int i = 0; i < numFrames; ++i) {
				mInputPtrs[c][i] = buffer[i * numChannels + c];
			}
		} else {
			std::fill(mInputPtrs[c], mInputPtrs[c] + numFrames, 0.0f);
		}
	}

	int outOffset = mAEffect->numInputs * numFrames;
	for (int c = 0; c < mAEffect->numOutputs; ++c) {
		mOutputPtrs[c] = &mProcessBuffer[outOffset + c * numFrames];
	}

	// 3. process MIDI & update note tracking
	if (!mIDIMessages.empty()) {
		// update active notes tracking
		for (const auto& msg : mIDIMessages) {
			uint8_t status = msg.status;
			uint8_t type = status & 0xF0;
			uint8_t channel = status & 0x0F;

			if (type == 0x90) { // note on
				if (msg.data2 > 0) {
					mActiveMIDINotes[channel].insert(msg.data1);
				} else {
					mActiveMIDINotes[channel].erase(msg.data1);
				}
			} else if (type == 0x80) { // note off
				mActiveMIDINotes[channel].erase(msg.data1);
			}
		}

		int numEvents = (int)mIDIMessages.size();
		const size_t kEventsOffset = (size_t)((char*)&((VstEvents*)nullptr)->events - (char*)nullptr);

		size_t ptrArraySize = numEvents * sizeof(VstEvent*);
		size_t dataSize = numEvents * sizeof(VstMidiEvent);
		size_t totalSize = kEventsOffset + ptrArraySize + dataSize;

		if (mVSTEventBuffer.size() < totalSize)
			mVSTEventBuffer.resize(totalSize);

		mVSTEvents = (VstEvents*)mVSTEventBuffer.data();
		mVSTEvents->numEvents = numEvents;
		mVSTEvents->reserved = 0;

		uint8_t* eventDataStart = mVSTEventBuffer.data() + kEventsOffset + ptrArraySize;

		for (int i = 0; i < numEvents; ++i) {
			VstMidiEvent* vme = (VstMidiEvent*)(eventDataStart + i * sizeof(VstMidiEvent));
			vme->type = kVstMidiType;
			vme->byteSize = sizeof(VstMidiEvent);
			vme->deltaFrames = mIDIMessages[i].frameIndex;
			vme->flags = 0;
			vme->noteLength = 0;
			vme->noteOffset = 0;
			vme->detune = 0;
			vme->noteOffVelocity = 0;
			vme->reserved1 = 0;
			vme->reserved2 = 0;
			vme->midiData[0] = mIDIMessages[i].status;
			vme->midiData[1] = mIDIMessages[i].data1;
			vme->midiData[2] = mIDIMessages[i].data2;
			vme->midiData[3] = 0;

			mVSTEvents->events[i] = (VstEvent*)vme;
		}

		mAEffect->dispatcher(mAEffect, effProcessEvents, 0, 0, mVSTEvents, 0.0f);
	}

	// 4. process audio
	mAEffect->processReplacing(mAEffect,
							   (mAEffect->numInputs > 0) ? mInputPtrs.data() : nullptr,
							   (mAEffect->numOutputs > 0) ? mOutputPtrs.data() : nullptr,
							   numFrames);

	// 5. interleave output
	for (int i = 0; i < numFrames; ++i) {
		for (int c = 0; c < numChannels; ++c) {
			float val = 0.0f;
			if (c < mAEffect->numOutputs) {
				val = mOutputPtrs[c][i];
			} else if (c == 1 && mAEffect->numOutputs == 1) {
				val = mOutputPtrs[0][i];
			}
			buffer[i * numChannels + c] = val;
		}
	}
}

void VSTProcessor::Save(std::ostream& out) {
	out << "VST_BEGIN\n";
	out << "PATH \"" << mPath << "\"\n";

	if (mAEffect) {
		if (mAEffect->flags & effFlagsProgramChunks) {
			void* chunkData = nullptr;
			bool isPreset = false;
			// request entire bank first
			VstIntPtr len = mAEffect->dispatcher(mAEffect, effGetChunk, 0, 0, &chunkData, 0.0f);
			if (len == 0 || !chunkData) {
				// fallback: request current program
				len = mAEffect->dispatcher(mAEffect, effGetChunk, 1, 0, &chunkData, 0.0f);
				isPreset = true;
			}
			if (len > 0 && chunkData) {
				std::string encoded = Base64Encode((unsigned char*)chunkData, (unsigned int)len);
				if (isPreset)
					out << "CHUNK_PROG " << encoded << "\n";
				else
					out << "CHUNK " << encoded << "\n";
			}
		}
	}

	out << "PARAMS_BEGIN\n";
	for (int i = 0; i < (int)mParameters.size(); ++i) {
		out << "P " << i << " " << mParameters[i]->value << "\n";
	}
	out << "PARAMS_END\n";

	out << "VST_END\n";
}

void VSTProcessor::Load(std::istream& in) {
	std::string line;
	bool loadedChunk = false;
	while (std::getline(in, line)) {
		if (line == "VST_END")
			break;

		if (line.rfind("PATH ", 0) == 0) {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos) {
				mPath = line.substr(q1 + 1, q2 - q1 - 1);
				if (!mPath.empty()) {
					if (!Load()) {
						std::cout << "Failed to load VST from serialized path: " << mPath << "\n";
					}
				}
			}
		} else if (line.rfind("CHUNK ", 0) == 0 || line.rfind("CHUNK_PROG ", 0) == 0) {
			bool isPreset = (line.rfind("CHUNK_PROG ", 0) == 0);
			std::string b64 = line.substr(isPreset ? 11 : 6);
			std::vector<unsigned char> data = Base64Decode(b64);
			if (mAEffect && !data.empty()) {
				mAEffect->dispatcher(mAEffect, effSetChunk, isPreset ? 1 : 0, (VstIntPtr)data.size(), data.data(), 0.0f);
				InitializeParameters(); // re-sync local values without disturbing
				loadedChunk = true;
			}
		} else if (line.rfind("P ", 0) == 0) {
			int idx;
			float val;
			if (sscanf_s(line.c_str(), "P %d %f", &idx, &val) == 2) {
				if (mAEffect && idx < mAEffect->numParams) {
					// only push old parameters locally if we didn't just load a complete macro chunk
					if (!loadedChunk) {
						mAEffect->setParameter(mAEffect, idx, val);
						if (idx < (int)mParameters.size()) {
							mParameters[idx]->value = val;
							if (idx < (int)mLastSentValues.size())
								mLastSentValues[idx] = val;
						}
					}
				}
			}
		}
	}
}

// editor support (windows)
#ifdef _WIN32

LRESULT CALLBACK VSTProcessor::EditorWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_KEYDOWN) {
		if (!(lParam & (1 << 30))) {
			if (VSTProcessor::OnGlobalKeyEvent) {
				VSTProcessor::OnGlobalKeyEvent((int)wParam, true);
			}
		}
	} else if (msg == WM_KEYUP) {
		if (VSTProcessor::OnGlobalKeyEvent) {
			VSTProcessor::OnGlobalKeyEvent((int)wParam, false);
		}
	}

	if (msg == WM_CLOSE) {
		VSTProcessor* proc = (VSTProcessor*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (proc)
			proc->CloseEditor();
		return 0;
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool VSTProcessor::HasEditor() const {
	return (mAEffect && (mAEffect->flags & effFlagsHasEditor));
}

void VSTProcessor::OpenEditor(void* parentWindowHandle) {
	if (!HasEditor() || mEditorWindow)
		return;

	DPI_AWARENESS_CONTEXT oldContext = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	WNDCLASSEX wc = {0};
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = EditorWindowProc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = "VSTEditorClass";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	RegisterClassEx(&wc);

	ERect* rect = nullptr;
	mAEffect->dispatcher(mAEffect, effEditGetRect, 0, 0, &rect, 0.0f);
	int width = 400;
	int height = 300;
	if (rect) {
		width = rect->right - rect->left;
		height = rect->bottom - rect->top;
	}

	RECT wr = {0, 0, width, height};
	AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hParent = (HWND)parentWindowHandle;

	mEditorWindow = CreateWindowEx(0, "VSTEditorClass", ("Editor: " + mName).c_str(),
								   WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
								   wr.right - wr.left, wr.bottom - wr.top,
								   hParent, NULL, GetModuleHandle(NULL), NULL);

	SetWindowLongPtr(mEditorWindow, GWLP_USERDATA, (LONG_PTR)this);

	mAEffect->dispatcher(mAEffect, effEditOpen, 0, 0, mEditorWindow, 0.0f);

	ShowWindow(mEditorWindow, SW_SHOW);
	UpdateWindow(mEditorWindow);

	SetThreadDpiAwarenessContext(oldContext);
}

void VSTProcessor::CloseEditor() {
	if (mEditorWindow && mAEffect) {
		mAEffect->dispatcher(mAEffect, effEditClose, 0, 0, 0, 0.0f);
		DestroyWindow(mEditorWindow);
		mEditorWindow = nullptr;
	}
}

bool VSTProcessor::IsEditorOpen() const {
	return mEditorWindow != nullptr;
}

#else
bool VSTProcessor::HasEditor() const {
	return false;
}
void VSTProcessor::OpenEditor(void* parentWindowHandle) {}
void VSTProcessor::CloseEditor() {}
bool VSTProcessor::IsEditorOpen() const {
	return false;
}
#endif

// host callback
VstIntPtr VSTCALLBACK VSTProcessor::HostCallback(AEffect* effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt) {
	VSTProcessor* proc = nullptr;
	{
		std::lock_guard<std::mutex> lock(gEffectMapMutex);
		auto it = gEffectMap.find(effect);
		if (it != gEffectMap.end())
			proc = it->second;
	}

	switch (opcode) {
	case audioMasterVersion:
		return 2400;
	case audioMasterIdle:
		if (proc && proc->IsEditorOpen()) {
			proc->mAEffect->dispatcher(proc->mAEffect, effEditIdle, 0, 0, 0, 0.0f);
		}
		return 1;
	case audioMasterAutomate:
		if (proc && index >= 0 && index < (int)proc->mParameters.size()) {
			proc->mParameters[index]->value = opt;
			if (index < (int)proc->mLastSentValues.size()) {
				proc->mLastSentValues[index] = opt;
			}
		}
		return 1;
	case audioMasterGetTime:
		if (proc) {
			return (VstIntPtr)&proc->mTimeInfo;
		}
		return 0;
	case audioMasterGetSampleRate:
		return 48000;
	case audioMasterGetBlockSize:
		return 512;
	case audioMasterGetCurrentProcessLevel:
		return 2; // kVstProcessLevelRealtime
	case audioMasterGetVendorString:
		if (ptr)
			strcpy_s((char*)ptr, 64, "MSDAW");
		return 1;
	case audioMasterGetProductString:
		if (ptr)
			strcpy_s((char*)ptr, 64, "MSDAW Host");
		return 1;
	case audioMasterGetVendorVersion:
		return 1000;
	case audioMasterCanDo:
		if (ptr) {
			std::string canDo = (const char*)ptr;
			if (canDo == "sendVstEvents" ||
				canDo == "sendVstMidiEvent" ||
				canDo == "sendVstTimeInfo" ||
				canDo == "receiveVstEvents" ||
				canDo == "receiveVstMidiEvent" ||
				canDo == "supplyIdle" ||
				canDo == "sizeWindow") {
				return 1;
			}
		}
		return 0;
	default:
		break;
	}
	return 0;
}
