#pragma once
#include "AudioProcessor.h"
#include "ProcessorHost.h"
#include "imgui.h" // for ImU32
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Parameter;

// ================================================================
// RACK CHAIN
// ================================================================
// one parallel branch of a rack. every chain is handed the same input the rack was
// handed, runs its own devices in series over its own copy of it, and its output is
// summed with every other chain's - so two chains carrying the same audio sum to +6 dB,
// exactly as they do in the reference product
class RackChain : public ProcessorHost {
public:
	RackChain();
	~RackChain() override;

	// stable per-chain identity, unique within a session and preserved by save/load.
	// the chain's mixer parameters are named after it ("Chain 7 Volume"), because
	// automation is bound by parameter name and a chain's position in the list is not
	// stable enough to name anything by
	uint32_t GetId() const { return mId; }
	void AdoptId(uint32_t id);

	std::vector<std::shared_ptr<AudioProcessor>>& GetProcessors() override { return mProcessors; }

	const std::string& GetName() const { return mName; }
	void SetName(const std::string& name) { mName = name; }

	// 0 means "inherit": the view draws the chain in the rack's color
	ImU32 GetColor() const { return mColor; }
	void SetColor(ImU32 color) { mColor = color; }

	bool GetMute() const { return mMute; }
	void SetMute(bool mute) { mMute = mute; }
	bool GetSolo() const { return mSolo; }
	void SetSolo(bool solo) { mSolo = solo; }

	Parameter* GetVolumeParameter() const { return mVolumeParam.get(); }
	Parameter* GetPanParameter() const { return mPanParam.get(); }

	void PrepareToPlay(double sampleRate);
	void Reset();
	void AllNotesOff();
	bool HasInstrument() const;

	// runs this chain's devices over `buffer` in place, then its own fader
	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages, const ProcessContext& context);

	void Save(std::ostream& out);
	void Load(std::istream& in);
protected:
	void CollectOwnParameters(std::vector<Parameter*>& out) override;
private:
	// the mixer parameters carry the chain id in their names, so they are renamed
	// whenever the id changes (which only happens on load)
	void RenameMixerParameters();

	uint32_t mId = 0;
	std::string mName = "Chain";
	ImU32 mColor = 0;
	bool mMute = false;
	bool mSolo = false;

	std::unique_ptr<Parameter> mVolumeParam; // dB
	std::unique_ptr<Parameter> mPanParam;    // -1 to 1

	std::vector<std::shared_ptr<AudioProcessor>> mProcessors;
};

// ================================================================
// RACK
// ================================================================
// a device that contains devices: it holds parallel chains and a bank of macro knobs,
// each of which drives any number of parameters anywhere inside it over a min/max range
//
// the macros are ordinary Parameters on the rack ("Macro 1" ... "Macro 16"), which is
// what makes them automatable and serializable with no special cases - a macro's
// user-facing title is a separate, purely cosmetic field, so renaming one can never
// orphan the automation curve bound to it
class RackProcessor : public AudioProcessor {
public:
	// the reference product shows eight macro knobs by default and allows up to
	// sixteen. all sixteen exist from construction: the parameter list an automation
	// curve binds into must not change shape when the user reveals another knob
	static constexpr int kMaxMacros = 16;
	static constexpr int kDefaultMacroCount = 8;
	static constexpr float kMacroMax = 127.0f;

	// one parameter a macro drives. the device is held weakly and re-resolved rather
	// than pointed at: an undo can take a device out of the rack and put it back, and
	// a mapping has to follow it without ever writing into a parameter that has left
	struct MacroMapping {
		std::weak_ptr<AudioProcessor> device;
		std::string paramName;
		float minValue = 0.0f;
		float maxValue = 1.0f;
		Parameter* resolved = nullptr; // cache, rebuilt when the chain generation moves
	};

	struct Macro {
		std::string title; // display only; empty falls back to the parameter's name
		ImU32 color = 0;   // 0 means "inherit": drawn in the theme accent
		std::vector<MacroMapping> mappings;
	};

	// everything a rack-level edit changes, snapshotted whole for the undo history.
	// the chains are held by shared_ptr, so undoing a chain deletion brings its devices
	// back with it
	struct RackState {
		std::string name;
		ImU32 color = 0;
		int visibleMacroCount = kDefaultMacroCount;
		std::vector<Macro> macros;
		std::vector<std::shared_ptr<RackChain>> chains;
	};

	RackProcessor();
	~RackProcessor() override;

	const char* GetName() const override { return mName.c_str(); }
	std::string GetProcessorId() const override { return "Rack"; }
	// a rack holding an instrument anywhere inside it IS one, so the track's note-off
	// sweep and the "this track can play notes" test both reach into it
	bool IsInstrument() const override;

	void PrepareToPlay(double sampleRate) override;
	void Reset() override;
	void AllNotesOff() override;
	void Process(float* buffer, int numFrames, int numChannels,
				 std::vector<MIDIMessage>& mIDIMessages,
				 const ProcessContext& context) override;

	void CollectHostedChains(std::vector<ProcessorHost*>& out) override;
	void CopyStateFrom(const AudioProcessor& other) override;

	void Save(std::ostream& out) override;
	void Load(std::istream& in) override;

	// ---- identity ----
	void SetName(const std::string& name) { mName = name; }
	ImU32 GetColor() const { return mColor; } // 0 means "inherit the theme's device color"
	void SetColor(ImU32 color) { mColor = color; }

	// ---- chains ----
	std::vector<std::shared_ptr<RackChain>>& GetChains() { return mChains; }
	std::shared_ptr<RackChain> AddChain(const std::string& name);
	void InsertChain(int index, std::shared_ptr<RackChain> chain);
	void RemoveChain(int index);

	// ---- macros ----
	int GetVisibleMacroCount() const { return mVisibleMacroCount; }
	void SetVisibleMacroCount(int count);
	Parameter* GetMacroParameter(int index) const;
	const Macro& GetMacro(int index) const { return mMacros[index]; }
	Macro& GetMacroMutable(int index) { return mMacros[index]; }

	// maps a parameter to a macro over the range the caller asks for - the reference
	// product opens a fresh mapping at the parameter's own full range. re-mapping the
	// same parameter to the same macro updates that entry instead of stacking a second
	void MapMacro(int macroIndex, const std::shared_ptr<AudioProcessor>& device,
				  const std::string& paramName, float minValue, float maxValue);
	void UnmapMacro(int macroIndex, int mappingIndex);
	// true when a macro drives this parameter, and so it is no longer the user's to turn
	bool IsParameterMapped(const Parameter* parameter);

	// the device inside this rack that owns `parameter`, for turning a click on a knob
	// into a mapping. null when the parameter belongs to something else entirely
	std::shared_ptr<AudioProcessor> FindDeviceOwning(const Parameter* parameter) const;

	// ---- view state ----
	// which of the rack's three panels are up, the same kind of state a track's collapse
	// flag is: the user set it, it belongs to the document, and it is saved with it
	bool mShowMacros = true;
	bool mShowChains = true;
	bool mShowDevices = true;

	// the chain whose devices the rack is showing, always clamped into the chain list a
	// caller is about to index with it
	int GetSelectedChainIndex() const;
	void SetSelectedChainIndex(int index) { mSelectedChain = index; }
	std::shared_ptr<RackChain> GetSelectedChain();

	// ---- undo support ----
	RackState CaptureState() const;
	void ApplyState(const RackState& state);
private:
	// re-resolves every macro mapping if any chain in the session has changed since the
	// last check. called at the top of Process (a plain load in the common case) and by
	// the view before it draws a mapping
	void RefreshMacroBindings();
	void ApplyMacros();

	// dotted chain/device indices ("0.2.1.0"), the only stable way to name a device
	// inside the rack in a file format that has no per-device ids
	std::string PathOfDevice(const std::shared_ptr<AudioProcessor>& device) const;
	std::shared_ptr<AudioProcessor> DeviceAtPath(const std::string& path) const;

	std::string mName = "Audio Effect Rack";
	ImU32 mColor = 0;
	int mVisibleMacroCount = kDefaultMacroCount;

	std::vector<Macro> mMacros;
	std::vector<Parameter*> mMacroParams; // owned by mParameters, in macro order
	std::vector<std::shared_ptr<RackChain>> mChains;

	int mSelectedChain = 0;
	uint32_t mBoundGeneration = 0;

	// ---- audio scratch ----
	// grown on the audio thread the first time a block of a given size arrives and kept
	// afterwards, the same way a track grows its group accumulator
	std::vector<float> mDryBuffer;
	std::vector<float> mSumBuffer;
	std::vector<float> mChainBuffer;
	std::vector<MIDIMessage> mChainMIDI;
};
