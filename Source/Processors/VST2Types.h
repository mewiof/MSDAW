#pragma once
#include <cstdint>

// ================================================================
// VST2 PLUGIN INTERFACE
// ================================================================

// the slice of the VST 2.4 binary interface MSDAW speaks as a host, written
// from the protocol itself rather than taken from steinberg's SDK, which we hold
// no right to redistribute. the names are ours. the numbers and layouts are what
// every compiled plugin expects, so they are fixed -- Tests/VST2TypesTests.cpp
// pins them
//
// NOTE: plugins call and are called with cdecl. it is the compiler's default
// (and x64's only convention), so no declaration here spells it out

namespace vst2 {

struct Effect;

// what the host asks of a plugin, through Effect::dispatcher. a plugin answers
// anything it does not handle with 0
enum class EffectOpcode : int32_t {
	Open = 0,
	Close = 1,
	GetParameterLabel = 6,
	GetParameterDisplay = 7,
	GetParameterName = 8,
	SetSampleRate = 10, // rate in opt
	SetBlockSize = 11,	// size in value
	MainsChanged = 12,	// value 1 resumes processing, 0 suspends it
	EditorGetRect = 13, // ptr receives a Rect*
	EditorOpen = 14,	// ptr is the parent window
	EditorClose = 15,
	EditorIdle = 19,
	GetChunk = 23,		// index 0 asks for the bank, 1 the current program. ptr receives the data, the size comes back
	SetChunk = 24,		// same index, size in value
	ProcessEvents = 25, // ptr is an Events*
	GetCategory = 35,
	GetEffectName = 45,
};

// what a plugin asks of the host, through the HostCallback it was created with
enum class HostOpcode : int32_t {
	Automate = 0, // the plugin moved a parameter itself: index, new value in opt
	Version = 1,
	Idle = 3,
	GetTime = 7,	 // answered with a TimeInfo*
	SizeWindow = 15, // width in index, height in value
	GetSampleRate = 16,
	GetBlockSize = 17,
	GetCurrentProcessLevel = 23,
	GetVendorString = 32,
	GetProductString = 33,
	GetVendorVersion = 34,
	CanDo = 37, // ptr names the capability
	BeginEdit = 43,
	EndEdit = 44,
};

using HostCallback = intptr_t (*)(Effect* effect, HostOpcode opcode, int32_t index, intptr_t value, void* ptr, float opt);

// what a plugin binary exports, as VSTPluginMain or, in older ones, main
using PluginMain = Effect* (*)(HostCallback host);

// a plugin instance as the host sees it. the entry point returns one, and every
// call into the plugin passes it back as the first argument
struct Effect {
	int32_t magic; // kEffectMagic
	intptr_t (*dispatcher)(Effect* effect, EffectOpcode opcode, int32_t index, intptr_t value, void* ptr, float opt);
	void (*processAccumulating)(Effect* effect, float** inputs, float** outputs, int32_t numFrames); // gone by 2.4
	void (*setParameter)(Effect* effect, int32_t index, float value);
	float (*getParameter)(Effect* effect, int32_t index);
	int32_t numPrograms;
	int32_t numParameters;
	int32_t numInputs;
	int32_t numOutputs;
	int32_t flags; // kEffectHasEditor, ...
	intptr_t reserved1;
	intptr_t reserved2;
	int32_t initialDelay; // latency in samples
	int32_t realQualities; // unused
	int32_t offQualities;  // unused
	float ioRatio;		   // unused
	void* object;		   // the plugin's own
	void* user;			   // free for the host
	int32_t uniqueID;
	int32_t version;
	void (*processReplacing)(Effect* effect, float** inputs, float** outputs, int32_t numFrames);
	void (*processDoubleReplacing)(Effect* effect, double** inputs, double** outputs, int32_t numFrames);
	char future[56];
};

// an editor's bounds in pixels, as EditorGetRect reports them
struct Rect {
	int16_t top;
	int16_t left;
	int16_t bottom;
	int16_t right;
};

// the header every event starts with. type says which layout the rest follows
struct Event {
	int32_t type;
	int32_t byteSize;
	int32_t deltaFrames; // offset into the block being processed
	int32_t flags;
	uint8_t data[16];
};

struct MIDIEvent {
	int32_t type;	  // kEventTypeMIDI
	int32_t byteSize; // sizeof(MIDIEvent)
	int32_t deltaFrames;
	int32_t flags;
	int32_t noteLength;
	int32_t noteOffset;
	uint8_t message[4]; // status, data1, data2, then a zero
	int8_t detune;		// cents
	uint8_t noteOffVelocity;
	uint8_t reserved1;
	uint8_t reserved2;
};

// the batch ProcessEvents delivers. events runs past its two declared entries:
// whoever sends the batch allocates room for numEvents pointers
struct Events {
	int32_t numEvents;
	intptr_t reserved;
	Event* events[2];
};

// the transport as a plugin pulls it with HostOpcode::GetTime. flags says which
// of the optional fields are filled in
struct TimeInfo {
	double samplePosition;
	double sampleRate;
	double nanoseconds;
	double ppqPosition;
	double tempo;
	double barStartPosition;
	double cycleStartPosition;
	double cycleEndPosition;
	int32_t timeSignatureNumerator;
	int32_t timeSignatureDenominator;
	int32_t smpteOffset;
	int32_t smpteFrameRate;
	int32_t samplesToNextClock;
	int32_t flags; // kTransportPlaying, ...
};

// what every Effect::magic holds: the characters 'VstP'
constexpr int32_t kEffectMagic = ('V' << 24) | ('s' << 16) | ('t' << 8) | 'P';

// bits of Effect::flags
constexpr int32_t kEffectHasEditor = 1 << 0;
constexpr int32_t kEffectProgramChunks = 1 << 5; // state travels as an opaque chunk rather than as parameters
constexpr int32_t kEffectIsSynth = 1 << 8;

// what EffectOpcode::GetCategory answers for an instrument
constexpr intptr_t kCategorySynth = 2;

// values of Event::type
constexpr int32_t kEventTypeMIDI = 1;

// bits of TimeInfo::flags
constexpr int32_t kTransportPlaying = 1 << 1;
constexpr int32_t kPPQPositionValid = 1 << 9;
constexpr int32_t kTempoValid = 1 << 10;
constexpr int32_t kBarsValid = 1 << 11;
constexpr int32_t kTimeSignatureValid = 1 << 13;

// what HostOpcode::GetCurrentProcessLevel answers while the audio thread is calling
constexpr intptr_t kProcessLevelRealtime = 2;

} // namespace vst2
