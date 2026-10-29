#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <cstddef>

#include "Processors/VST2Types.h"

// ================================================================
// VST2 TYPES
// ================================================================

// NOTE: these fail the build, not a test run. every VST2 plugin was compiled
// against these layouts, and one that drifts by a byte fails nowhere -- the
// plugin just reads the wrong memory. the numbers were checked against
// steinberg's SDK when MSDAW stopped depending on it. only fields the host
// touches are pinned: a mistake in any other still moves one of them, or the size

using namespace vst2;

static_assert(sizeof(void*) == 8, "these are the 64-bit layouts, the only build MSDAW ships");

static_assert(sizeof(Effect) == 192);
static_assert(offsetof(Effect, magic) == 0);
static_assert(offsetof(Effect, dispatcher) == 8);
static_assert(offsetof(Effect, setParameter) == 24);
static_assert(offsetof(Effect, getParameter) == 32);
static_assert(offsetof(Effect, numParameters) == 44);
static_assert(offsetof(Effect, numInputs) == 48);
static_assert(offsetof(Effect, numOutputs) == 52);
static_assert(offsetof(Effect, flags) == 56);
static_assert(offsetof(Effect, processReplacing) == 120);
static_assert(offsetof(Effect, processDoubleReplacing) == 128);

static_assert(sizeof(Rect) == 8);
static_assert(offsetof(Rect, top) == 0);
static_assert(offsetof(Rect, left) == 2);
static_assert(offsetof(Rect, bottom) == 4);
static_assert(offsetof(Rect, right) == 6);

static_assert(sizeof(Event) == 32);

static_assert(sizeof(MIDIEvent) == 32);
static_assert(offsetof(MIDIEvent, type) == 0);
static_assert(offsetof(MIDIEvent, byteSize) == 4);
static_assert(offsetof(MIDIEvent, deltaFrames) == 8);
static_assert(offsetof(MIDIEvent, flags) == 12);
static_assert(offsetof(MIDIEvent, noteLength) == 16);
static_assert(offsetof(MIDIEvent, noteOffset) == 20);
static_assert(offsetof(MIDIEvent, message) == 24);
static_assert(offsetof(MIDIEvent, detune) == 28);
static_assert(offsetof(MIDIEvent, noteOffVelocity) == 29);
static_assert(offsetof(MIDIEvent, reserved1) == 30);
static_assert(offsetof(MIDIEvent, reserved2) == 31);

static_assert(sizeof(Events) == 32);
static_assert(offsetof(Events, numEvents) == 0);
static_assert(offsetof(Events, reserved) == 8);
static_assert(offsetof(Events, events) == 16);

static_assert(sizeof(TimeInfo) == 88);
static_assert(offsetof(TimeInfo, samplePosition) == 0);
static_assert(offsetof(TimeInfo, sampleRate) == 8);
static_assert(offsetof(TimeInfo, nanoseconds) == 16);
static_assert(offsetof(TimeInfo, ppqPosition) == 24);
static_assert(offsetof(TimeInfo, tempo) == 32);
static_assert(offsetof(TimeInfo, barStartPosition) == 40);
static_assert(offsetof(TimeInfo, cycleStartPosition) == 48);
static_assert(offsetof(TimeInfo, cycleEndPosition) == 56);
static_assert(offsetof(TimeInfo, timeSignatureNumerator) == 64);
static_assert(offsetof(TimeInfo, timeSignatureDenominator) == 68);
static_assert(offsetof(TimeInfo, smpteOffset) == 72);
static_assert(offsetof(TimeInfo, smpteFrameRate) == 76);
static_assert(offsetof(TimeInfo, samplesToNextClock) == 80);
static_assert(offsetof(TimeInfo, flags) == 84);
