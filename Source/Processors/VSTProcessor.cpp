#include "Parameters/SliderParameter.h"
#include "PrecompHeader.h"
#include "VSTProcessor.h"
#include "PluginManager.h"
#include "MIDIPanic.h"
#include <iostream>
#include <mutex>
#include <map>
#include <algorithm>
#include <cstddef>
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
static std::map<vst2::Effect*, VSTProcessor*> gEffectMap;

// initialize the static callback
std::function<void(int, bool)> VSTProcessor::OnGlobalKeyEvent = nullptr;

VSTProcessor::VSTProcessor(const std::string& path)
	: mPath(path) {
	memset(&mTimeInfo, 0, sizeof(mTimeInfo));
	mTimeInfo.sampleRate = 48000.0;
	mTimeInfo.tempo = 120.0;
	mTimeInfo.timeSignatureNumerator = 4;
	mTimeInfo.timeSignatureDenominator = 4;
}

VSTProcessor::~VSTProcessor() {
	CloseEditor();
	Suspend();

	// serialized against every other plugin binary touching the process: the scan
	// thread and the UI thread both arrive here (see PluginManager::BinaryLock)
	std::lock_guard<std::mutex> binaryLock(PluginManager::BinaryLock());

	if (mEffect) {
		{
			std::lock_guard<std::mutex> lock(gEffectMapMutex);
			gEffectMap.erase(mEffect);
		}
		mEffect->dispatcher(mEffect, vst2::EffectOpcode::Close, 0, 0, 0, 0.0f);
		mEffect = nullptr;
	}

#ifdef _WIN32
	if (mLibrary) {
		// FreeLibrary(mLibrary);
		mLibrary = nullptr;
	}
#endif
}

bool VSTProcessor::Load() {
	if (mEffect)
		return true;

	// serialized against every other plugin binary touching the process: the scan
	// thread and the UI thread both arrive here (see PluginManager::BinaryLock)
	std::lock_guard<std::mutex> binaryLock(PluginManager::BinaryLock());

#ifdef _WIN32
	mLibrary = LoadLibraryA(mPath.c_str());
	if (!mLibrary) {
		std::cout << "Failed to load VST DLL: " << mPath << " Error: " << GetLastError() << std::endl;
		return false;
	}

	vst2::PluginMain mainProc = (vst2::PluginMain)GetProcAddress(mLibrary, "VSTPluginMain");
	if (!mainProc)
		mainProc = (vst2::PluginMain)GetProcAddress(mLibrary, "main");

	if (!mainProc) {
		std::cout << "VST Entry point not found in " << mPath << std::endl;
		return false;
	}

	mEffect = mainProc(&VSTProcessor::HostCallback);
	if (!mEffect || mEffect->magic != vst2::kEffectMagic) {
		std::cout << "Invalid VST magic number." << std::endl;
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(gEffectMapMutex);
		gEffectMap[mEffect] = this;
	}

	mEffect->dispatcher(mEffect, vst2::EffectOpcode::Open, 0, 0, 0, 0.0f);

	// get name
	char nameBuf[64] = {0};
	mEffect->dispatcher(mEffect, vst2::EffectOpcode::GetEffectName, 0, 0, nameBuf, 0.0f);
	if (nameBuf[0])
		mName = nameBuf;
	else
		mName = "VST Plugin";

	// determine if this is an instrument
	if (mEffect->flags & vst2::kEffectIsSynth) {
		mIsSynth = true;
	} else {
		intptr_t category = mEffect->dispatcher(mEffect, vst2::EffectOpcode::GetCategory, 0, 0, 0, 0.0f);
		if (category == vst2::kCategorySynth) {
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

	for (int i = 0; i < mEffect->numParameters; ++i) {
		char paramName[64] = {0};
		char paramDisplay[64] = {0};
		char paramLabel[64] = {0};

		mEffect->dispatcher(mEffect, vst2::EffectOpcode::GetParameterName, i, 0, paramName, 0.0f);
		mEffect->dispatcher(mEffect, vst2::EffectOpcode::GetParameterDisplay, i, 0, paramDisplay, 0.0f);
		mEffect->dispatcher(mEffect, vst2::EffectOpcode::GetParameterLabel, i, 0, paramLabel, 0.0f);

		std::string name = paramName;
		if (name.empty())
			name = "Param " + std::to_string(i);

		float val = mEffect->getParameter(mEffect, i);
		AddParameter(std::make_unique<SliderParameter>(name, val, 0.0f, 1.0f));
		mLastSentValues.push_back(val);
	}
}

void VSTProcessor::PrepareToPlay(double sampleRate) {
	if (!mEffect)
		return;

	mEffect->dispatcher(mEffect, vst2::EffectOpcode::SetSampleRate, 0, 0, 0, (float)sampleRate);
	mEffect->dispatcher(mEffect, vst2::EffectOpcode::SetBlockSize, 0, 512, 0, 0.0f);

	Resume();
}

void VSTProcessor::Resume() {
	if (mEffect)
		mEffect->dispatcher(mEffect, vst2::EffectOpcode::MainsChanged, 0, 1, 0, 0.0f);
}

void VSTProcessor::Suspend() {
	if (mEffect)
		mEffect->dispatcher(mEffect, vst2::EffectOpcode::MainsChanged, 0, 0, 0, 0.0f);
}

void VSTProcessor::Reset() {
	// stop/seek: hard panic, also cut any reverb/delay tails for a clean restart
	mPendingPanic = true;
	mPendingPanicAllSoundOff = true;
}

void VSTProcessor::AllNotesOff() {
	// loop wrap: release held notes but let the plugin's own reverb/delay keep ringing
	mPendingPanic = true;
}

// a panic used to go out as a ProcessEvents dispatch of its own the moment the transport asked
// for it. that call sits outside the process cycle the events belong to, and the one
// Process makes a moment later can replace the queue before the plugin has read it, which
// leaves the notes hanging. queueing it instead lets it lead the next block's own events,
// which is also the only order that works: a wrap or a seek re-triggers a note at sample 0,
// and a release delivered behind that note-on would let go of the note we just started
void VSTProcessor::ApplyPendingPanic(std::vector<MIDIMessage>& mIDIMessages) {
	if (!mPendingPanic)
		return;

	PrependMIDIPanic(mIDIMessages, mActiveMIDINotes, mPendingPanicAllSoundOff);
	mPendingPanic = false;
	mPendingPanicAllSoundOff = false;
}

void VSTProcessor::Process(float* buffer, int numFrames, int numChannels,
						   std::vector<MIDIMessage>& mIDIMessages,
						   const ProcessContext& context) {
	if (!mEffect)
		return;

	mTimeInfo.samplePosition = context.currentSample;
	mTimeInfo.sampleRate = context.sampleRate;
	mTimeInfo.nanoseconds = 0;
	double seconds = context.currentSample / context.sampleRate;
	mTimeInfo.ppqPosition = seconds * (context.bpm / 60.0);
	mTimeInfo.tempo = context.bpm;
	mTimeInfo.barStartPosition = std::floor(mTimeInfo.ppqPosition / context.timeSigNumerator) * context.timeSigNumerator;
	mTimeInfo.cycleStartPosition = 0;
	mTimeInfo.cycleEndPosition = 0;
	mTimeInfo.timeSignatureNumerator = (int32_t)context.timeSigNumerator;
	mTimeInfo.timeSignatureDenominator = (int32_t)context.timeSigDenominator;
	mTimeInfo.smpteOffset = 0;
	mTimeInfo.smpteFrameRate = 0;
	mTimeInfo.samplesToNextClock = 0;

	mTimeInfo.flags = 0;
	mTimeInfo.flags |= vst2::kTempoValid | vst2::kTimeSignatureValid | vst2::kPPQPositionValid | vst2::kBarsValid;
	if (context.isPlaying)
		mTimeInfo.flags |= vst2::kTransportPlaying;

	// 1. sync parameters (bi-directional logic)
	if (mLastSentValues.size() < mParameters.size()) {
		mLastSentValues.resize(mParameters.size(), -1.0f);
	}

	for (int i = 0; i < mEffect->numParameters && i < (int)mParameters.size(); ++i) {
		float hostVal = mParameters[i]->value;
		float lastSent = mLastSentValues[i];

		if (std::abs(hostVal - lastSent) > 0.000001f) {
			mEffect->setParameter(mEffect, i, hostVal);
			mLastSentValues[i] = hostVal;
		}
		// note: if hostval == lastsent, we do nothing
	}

	// 2. prepare buffers
	int maxChannels = max(mEffect->numInputs, mEffect->numOutputs);
	int requiredBufferSize = numFrames * maxChannels * 2;
	if ((int)mProcessBuffer.size() < requiredBufferSize) {
		mProcessBuffer.resize(requiredBufferSize);
	}

	if (mInputPtrs.size() != (size_t)mEffect->numInputs)
		mInputPtrs.resize(mEffect->numInputs);
	if (mOutputPtrs.size() != (size_t)mEffect->numOutputs)
		mOutputPtrs.resize(mEffect->numOutputs);

	for (int c = 0; c < mEffect->numInputs; ++c) {
		mInputPtrs[c] = &mProcessBuffer[c * numFrames];
		if (c < numChannels) {
			for (int i = 0; i < numFrames; ++i) {
				mInputPtrs[c][i] = buffer[i * numChannels + c];
			}
		} else {
			std::fill(mInputPtrs[c], mInputPtrs[c] + numFrames, 0.0f);
		}
	}

	int outOffset = mEffect->numInputs * numFrames;
	for (int c = 0; c < mEffect->numOutputs; ++c) {
		mOutputPtrs[c] = &mProcessBuffer[outOffset + c * numFrames];
	}

	// 3. process MIDI & update note tracking
	ApplyPendingPanic(mIDIMessages);

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
		const size_t kEventsOffset = offsetof(vst2::Events, events);

		size_t ptrArraySize = numEvents * sizeof(vst2::Event*);
		size_t dataSize = numEvents * sizeof(vst2::MIDIEvent);
		size_t totalSize = kEventsOffset + ptrArraySize + dataSize;

		if (mVSTEventBuffer.size() < totalSize)
			mVSTEventBuffer.resize(totalSize);

		mVSTEvents = (vst2::Events*)mVSTEventBuffer.data();
		mVSTEvents->numEvents = numEvents;
		mVSTEvents->reserved = 0;

		uint8_t* eventDataStart = mVSTEventBuffer.data() + kEventsOffset + ptrArraySize;

		for (int i = 0; i < numEvents; ++i) {
			vst2::MIDIEvent* vme = (vst2::MIDIEvent*)(eventDataStart + i * sizeof(vst2::MIDIEvent));
			vme->type = vst2::kEventTypeMIDI;
			vme->byteSize = sizeof(vst2::MIDIEvent);
			vme->deltaFrames = mIDIMessages[i].frameIndex;
			vme->flags = 0;
			vme->noteLength = 0;
			vme->noteOffset = 0;
			vme->detune = 0;
			vme->noteOffVelocity = 0;
			vme->reserved1 = 0;
			vme->reserved2 = 0;
			vme->message[0] = mIDIMessages[i].status;
			vme->message[1] = mIDIMessages[i].data1;
			vme->message[2] = mIDIMessages[i].data2;
			vme->message[3] = 0;

			mVSTEvents->events[i] = (vst2::Event*)vme;
		}

		mEffect->dispatcher(mEffect, vst2::EffectOpcode::ProcessEvents, 0, 0, mVSTEvents, 0.0f);
	}

	// 4. process audio
	mEffect->processReplacing(mEffect,
							  (mEffect->numInputs > 0) ? mInputPtrs.data() : nullptr,
							  (mEffect->numOutputs > 0) ? mOutputPtrs.data() : nullptr,
							  numFrames);

	// 5. interleave output
	for (int i = 0; i < numFrames; ++i) {
		for (int c = 0; c < numChannels; ++c) {
			float val = 0.0f;
			if (c < mEffect->numOutputs) {
				val = mOutputPtrs[c][i];
			} else if (c == 1 && mEffect->numOutputs == 1) {
				val = mOutputPtrs[0][i];
			}
			buffer[i * numChannels + c] = val;
		}
	}
}

void VSTProcessor::Save(std::ostream& out) {
	out << "VST_BEGIN\n";
	out << "PATH \"" << mPath << "\"\n";

	if (mEffect) {
		if (mEffect->flags & vst2::kEffectProgramChunks) {
			void* chunkData = nullptr;
			bool isPreset = false;
			// request entire bank first
			intptr_t len = mEffect->dispatcher(mEffect, vst2::EffectOpcode::GetChunk, 0, 0, &chunkData, 0.0f);
			if (len == 0 || !chunkData) {
				// fallback: request current program
				len = mEffect->dispatcher(mEffect, vst2::EffectOpcode::GetChunk, 1, 0, &chunkData, 0.0f);
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
			if (mEffect && !data.empty()) {
				mEffect->dispatcher(mEffect, vst2::EffectOpcode::SetChunk, isPreset ? 1 : 0, (intptr_t)data.size(), data.data(), 0.0f);
				InitializeParameters(); // re-sync local values without disturbing
				loadedChunk = true;
			}
		} else if (line.rfind("P ", 0) == 0) {
			int idx;
			float val;
			if (sscanf_s(line.c_str(), "P %d %f", &idx, &val) == 2) {
				if (mEffect && idx < mEffect->numParameters) {
					// only push old parameters locally if we didn't just load a complete macro chunk
					if (!loadedChunk) {
						mEffect->setParameter(mEffect, idx, val);
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
	return (mEffect && (mEffect->flags & vst2::kEffectHasEditor));
}

void VSTProcessor::OpenEditor(void* parentWindowHandle) {
	if (!HasEditor() || mEditorWindow)
		return;

	HWND hParent = (HWND)parentWindowHandle;

	// native mode: create the window DPI-aware so the plugin renders 1:1 at the
	// display's real resolution (crisp, no DWM bitmap stretching / tearing). VST2
	// has no scale-negotiation, so such plugins draw at their native pixel size
	// (which may look small on a hi-DPI display). Scaled mode keeps the legacy
	// behavior: DPI-unaware, Windows stretches the window to match the DAW
	bool native = UseNativeEditorScaling();
	DPI_AWARENESS_CONTEXT wanted = native ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
										  : DPI_AWARENESS_CONTEXT_UNAWARE;
	DPI_AWARENESS_CONTEXT oldContext = SetThreadDpiAwarenessContext(wanted);

	WNDCLASSEX wc = {0};
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = EditorWindowProc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = "VSTEditorClass";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	RegisterClassEx(&wc);

	vst2::Rect* rect = nullptr;
	mEffect->dispatcher(mEffect, vst2::EffectOpcode::EditorGetRect, 0, 0, &rect, 0.0f);
	int width = 400;
	int height = 300;
	if (rect) {
		width = rect->right - rect->left;
		height = rect->bottom - rect->top;
	}

	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
	RECT wr = {0, 0, width, height};
	// size the non-client area for the target DPI so the client area exactly fits
	// the plugin view (prevents cropping on hi-DPI displays)
	UINT dpi = (native && hParent) ? GetDpiForWindow(hParent) : 96;
	AdjustWindowRectExForDpi(&wr, style, FALSE, 0, dpi);

	mEditorWindow = CreateWindowEx(0, "VSTEditorClass", ("Editor: " + mName).c_str(),
								   style, CW_USEDEFAULT, CW_USEDEFAULT,
								   wr.right - wr.left, wr.bottom - wr.top,
								   hParent, NULL, GetModuleHandle(NULL), NULL);

	SetWindowLongPtr(mEditorWindow, GWLP_USERDATA, (LONG_PTR)this);

	mEffect->dispatcher(mEffect, vst2::EffectOpcode::EditorOpen, 0, 0, mEditorWindow, 0.0f);

	// some plugins only report their true size after EditorOpen; re-query and fit
	vst2::Rect* rect2 = nullptr;
	mEffect->dispatcher(mEffect, vst2::EffectOpcode::EditorGetRect, 0, 0, &rect2, 0.0f);
	if (rect2) {
		int w2 = rect2->right - rect2->left;
		int h2 = rect2->bottom - rect2->top;
		if (w2 > 0 && h2 > 0 && (w2 != width || h2 != height)) {
			RECT wr2 = {0, 0, w2, h2};
			AdjustWindowRectExForDpi(&wr2, style, FALSE, 0, dpi);
			SetWindowPos(mEditorWindow, NULL, 0, 0, wr2.right - wr2.left, wr2.bottom - wr2.top,
						 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}

	ShowWindow(mEditorWindow, SW_SHOW);
	SetForegroundWindow(mEditorWindow);
	SetFocus(mEditorWindow);
	UpdateWindow(mEditorWindow);

	SetThreadDpiAwarenessContext(oldContext);
}

void VSTProcessor::EditorIdle() {
	// VST2 GUIs expect the host to pump EditorIdle regularly; without it many only
	// repaint on incidental mouse messages, so knob turns show partial/stale frames
	if (mEditorWindow && mEffect)
		mEffect->dispatcher(mEffect, vst2::EffectOpcode::EditorIdle, 0, 0, 0, 0.0f);
}

void VSTProcessor::CloseEditor() {
	if (mEditorWindow && mEffect) {
		// hide before destroy and reactivate the owner explicitly, otherwise
		// destroying a visible foreground owned window can minimize the main DAW
		HWND owner = GetWindow(mEditorWindow, GW_OWNER);
		mEffect->dispatcher(mEffect, vst2::EffectOpcode::EditorClose, 0, 0, 0, 0.0f);
		ShowWindow(mEditorWindow, SW_HIDE);
		DestroyWindow(mEditorWindow);
		mEditorWindow = nullptr;
		if (owner)
			SetActiveWindow(owner);
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
intptr_t VSTProcessor::HostCallback(vst2::Effect* effect, vst2::HostOpcode opcode, int32_t index, intptr_t value, void* ptr, float opt) {
	VSTProcessor* proc = nullptr;
	{
		std::lock_guard<std::mutex> lock(gEffectMapMutex);
		auto it = gEffectMap.find(effect);
		if (it != gEffectMap.end())
			proc = it->second;
	}

	switch (opcode) {
	case vst2::HostOpcode::Version:
		return 2400;
	case vst2::HostOpcode::Idle:
		if (proc && proc->IsEditorOpen()) {
			proc->mEffect->dispatcher(proc->mEffect, vst2::EffectOpcode::EditorIdle, 0, 0, 0, 0.0f);
		}
		return 1;
	case vst2::HostOpcode::SizeWindow:
#ifdef _WIN32
		if (proc && proc->IsEditorOpen()) {
			// index = requested client width, value = client height. size the
			// non-client area for the window's DPI so the client fits exactly
			HWND hwnd = (HWND)proc->mEditorWindow;
			RECT wr = {0, 0, index, (int)value};
			DWORD style = GetWindowLong(hwnd, GWL_STYLE);
			AdjustWindowRectExForDpi(&wr, style, FALSE, 0, GetDpiForWindow(hwnd));
			SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			return 1;
		}
#endif
		return 0;
	case vst2::HostOpcode::Automate:
		if (proc && index >= 0 && index < (int)proc->mParameters.size()) {
			proc->mParameters[index]->value = opt;
			if (index < (int)proc->mLastSentValues.size()) {
				proc->mLastSentValues[index] = opt;
			}
			// remember this as the last touched param so "Show Auto" targets it
			Parameter::NotifyExternalEdit(proc->mParameters[index].get());
			proc->CapturePanelParameter(index);
		}
		return 1;
	case vst2::HostOpcode::BeginEdit:
		// plugin GUI started a parameter gesture: capture the value for undo
		if (proc && index >= 0 && index < (int)proc->mParameters.size()) {
			proc->mParameters[index]->BeginEditGesture();
			// touching a control is enough to configure it onto the device panel
			proc->CapturePanelParameter(index);
		}
		return 1;
	case vst2::HostOpcode::EndEdit:
		// plugin GUI finished the gesture: commit one undo entry
		if (proc && index >= 0 && index < (int)proc->mParameters.size())
			proc->mParameters[index]->EndEditGesture();
		return 1;
	case vst2::HostOpcode::GetTime:
		if (proc) {
			return (intptr_t)&proc->mTimeInfo;
		}
		return 0;
	case vst2::HostOpcode::GetSampleRate:
		return 48000;
	case vst2::HostOpcode::GetBlockSize:
		return 512;
	case vst2::HostOpcode::GetCurrentProcessLevel:
		return vst2::kProcessLevelRealtime;
	case vst2::HostOpcode::GetVendorString:
		if (ptr)
			strcpy_s((char*)ptr, 64, "MSDAW");
		return 1;
	case vst2::HostOpcode::GetProductString:
		if (ptr)
			strcpy_s((char*)ptr, 64, "MSDAW Host");
		return 1;
	case vst2::HostOpcode::GetVendorVersion:
		return 1000;
	case vst2::HostOpcode::CanDo:
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
