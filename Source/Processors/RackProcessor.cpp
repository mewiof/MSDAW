#include "PrecompHeader.h"
#include "RackProcessor.h"

#include "Mixing.h"
#include "Parameters/KnobParameter.h"
#include "Parameters/SliderParameter.h"
#include "ProcessorFactory.h"
#include "ProcessorIO.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <sstream>

REGISTER_PROCESSOR(RackProcessor, "Rack", false)

// ================================================================
// CHAIN IDENTITY
// ================================================================

// hands out RackChain::mId. the same scheme Track uses: ids only have to be unique
// within a session, and loading a project adopts the saved ids and pushes the counter
// past them, so a chain created afterwards can never collide with one an automation
// curve was saved against
static std::atomic<uint32_t> sNextChainId{1};

static uint32_t AllocateChainId() {
	return sNextChainId.fetch_add(1);
}

static void ReserveChainId(uint32_t id) {
	uint32_t expected = sNextChainId.load();
	while (id >= expected && !sNextChainId.compare_exchange_weak(expected, id + 1)) {
	}
}

// ================================================================
// SUBTREE WALKS
// ================================================================

// every device in these chains, nested racks included, in the order they are saved in.
// the callback stops the walk by returning true
static bool VisitDevices(const std::vector<std::shared_ptr<RackChain>>& chains,
						 const std::function<bool(const std::shared_ptr<AudioProcessor>&)>& visit) {
	for (const auto& chain : chains) {
		if (!chain)
			continue;
		for (const auto& device : chain->GetProcessors()) {
			if (!device)
				continue;
			if (visit(device))
				return true;
			if (auto* nested = dynamic_cast<RackProcessor*>(device.get())) {
				if (VisitDevices(nested->GetChains(), visit))
					return true;
			}
		}
	}
	return false;
}

// ================================================================
// RACK CHAIN
// ================================================================

RackChain::RackChain() {
	mId = AllocateChainId();
	mVolumeParam = std::make_unique<SliderParameter>("", 0.0f, -60.0f, 6.0f);
	mPanParam = std::make_unique<SliderParameter>("", 0.0f, -1.0f, 1.0f);
	RenameMixerParameters();
}

RackChain::~RackChain() {}

void RackChain::AdoptId(uint32_t id) {
	if (id == 0)
		return;
	mId = id;
	ReserveChainId(id);
	RenameMixerParameters();
}

void RackChain::RenameMixerParameters() {
	mVolumeParam->name = "Chain " + std::to_string(mId) + " Volume";
	mPanParam->name = "Chain " + std::to_string(mId) + " Pan";
}

void RackChain::CollectOwnParameters(std::vector<Parameter*>& out) {
	out.push_back(mVolumeParam.get());
	out.push_back(mPanParam.get());
}

void RackChain::PrepareToPlay(double sampleRate) {
	for (auto& device : mProcessors)
		device->PrepareToPlay(sampleRate);
}

void RackChain::Reset() {
	for (auto& device : mProcessors)
		device->Reset();
}

void RackChain::AllNotesOff() {
	// same split as a track's: only instruments hold notes, and an effect's tail has to
	// ring on across the event
	for (auto& device : mProcessors) {
		if (device->IsInstrument())
			device->AllNotesOff();
	}
}

bool RackChain::HasInstrument() const {
	for (const auto& device : mProcessors) {
		if (device->IsInstrument())
			return true;
	}
	return false;
}

void RackChain::Process(float* buffer, int numFrames, int numChannels,
						std::vector<MIDIMessage>& mIDIMessages, const ProcessContext& context) {
	for (auto& device : mProcessors) {
		if (!device->IsBypassed())
			device->Process(buffer, numFrames, numChannels, mIDIMessages, context);
	}
	ApplyGainAndPan(buffer, numFrames, numChannels, mVolumeParam->value, mPanParam->value);
}

void RackChain::Save(std::ostream& out) {
	out << "CHAIN_BEGIN\n";
	out << "CHAIN_ID " << mId << "\n";
	out << "CHAIN_NAME \"" << mName << "\"\n";
	out << "CHAIN_COLOR " << mColor << "\n";
	out << "CHAIN_MUTE " << (mMute ? 1 : 0) << "\n";
	out << "CHAIN_SOLO " << (mSolo ? 1 : 0) << "\n";
	out << "CHAIN_VOL " << mVolumeParam->value << "\n";
	out << "CHAIN_PAN " << mPanParam->value << "\n";
	for (auto& device : mProcessors)
		ProcessorIO::SaveProcessor(out, *device);
	out << "CHAIN_END\n";
}

void RackChain::Load(std::istream& in) {
	std::string line;
	while (std::getline(in, line)) {
		if (line == "CHAIN_END")
			break;

		std::stringstream ss(line);
		std::string token;
		ss >> token;

		if (token == "CHAIN_ID") {
			uint32_t id = 0;
			ss >> id;
			AdoptId(id);
		} else if (token == "CHAIN_NAME") {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos)
				mName = line.substr(q1 + 1, q2 - q1 - 1);
		} else if (token == "CHAIN_COLOR") {
			ss >> mColor;
		} else if (token == "CHAIN_MUTE") {
			int value = 0;
			ss >> value;
			mMute = (value != 0);
		} else if (token == "CHAIN_SOLO") {
			int value = 0;
			ss >> value;
			mSolo = (value != 0);
		} else if (token == "CHAIN_VOL") {
			ss >> mVolumeParam->value;
		} else if (token == "CHAIN_PAN") {
			ss >> mPanParam->value;
		} else if (token == "PROCESSOR") {
			std::string typeId;
			ss >> typeId;
			if (auto device = ProcessorIO::LoadProcessor(in, typeId))
				AddProcessor(std::move(device));
		}
	}
}

// ================================================================
// RACK
// ================================================================

RackProcessor::RackProcessor() {
	mMacros.resize(kMaxMacros);
	mMacroParams.reserve(kMaxMacros);
	for (int i = 0; i < kMaxMacros; ++i) {
		// the name is the serialization and automation key and never changes; the title
		// the user types is cosmetic and lives beside it in mMacros
		std::string name = "Macro " + std::to_string(i + 1);
		mMacroParams.push_back(AddParameter(std::make_unique<KnobParameter>(name, 0.0f, 0.0f, kMacroMax)));
	}
}

RackProcessor::~RackProcessor() {}

bool RackProcessor::IsInstrument() const {
	for (const auto& chain : mChains) {
		for (const auto& device : chain->GetProcessors()) {
			if (device->IsInstrument())
				return true;
		}
	}
	return false;
}

void RackProcessor::PrepareToPlay(double sampleRate) {
	for (auto& chain : mChains)
		chain->PrepareToPlay(sampleRate);
}

void RackProcessor::Reset() {
	for (auto& chain : mChains)
		chain->Reset();
}

void RackProcessor::AllNotesOff() {
	for (auto& chain : mChains)
		chain->AllNotesOff();
}

void RackProcessor::CollectHostedChains(std::vector<ProcessorHost*>& out) {
	for (auto& chain : mChains)
		out.push_back(chain.get());
}

void RackProcessor::Process(float* buffer, int numFrames, int numChannels,
							std::vector<MIDIMessage>& mIDIMessages,
							const ProcessContext& context) {
	RefreshMacroBindings();
	ApplyMacros();

	// an empty rack is a plain pass-through, not a mute
	if (mChains.empty())
		return;

	const int sampleCount = numFrames * numChannels;
	if ((int)mDryBuffer.size() < sampleCount)
		mDryBuffer.resize(sampleCount);
	if ((int)mSumBuffer.size() < sampleCount)
		mSumBuffer.resize(sampleCount);
	if ((int)mChainBuffer.size() < sampleCount)
		mChainBuffer.resize(sampleCount);

	std::copy(buffer, buffer + sampleCount, mDryBuffer.begin());
	std::fill(mSumBuffer.begin(), mSumBuffer.begin() + sampleCount, 0.0f);

	bool anySolo = false;
	for (const auto& chain : mChains) {
		if (chain->GetSolo()) {
			anySolo = true;
			break;
		}
	}

	for (auto& chain : mChains) {
		if (chain->GetMute() || (anySolo && !chain->GetSolo()))
			continue;

		std::copy(mDryBuffer.begin(), mDryBuffer.begin() + sampleCount, mChainBuffer.begin());
		// every chain is handed the same notes. assign keeps the capacity the last block
		// grew, so a steady state allocates nothing
		mChainMIDI.assign(mIDIMessages.begin(), mIDIMessages.end());

		chain->Process(mChainBuffer.data(), numFrames, numChannels, mChainMIDI, context);

		for (int i = 0; i < sampleCount; ++i)
			mSumBuffer[i] += mChainBuffer[i];
	}

	// NOTE: what a chain did to its copy of the note list stays inside the rack. merging
	// several chains' edits back into one stream has no defensible answer, so devices
	// after the rack see the notes exactly as they arrived
	std::copy(mSumBuffer.begin(), mSumBuffer.begin() + sampleCount, buffer);
}

// ---- chains ----

std::shared_ptr<RackChain> RackProcessor::AddChain(const std::string& name) {
	auto chain = std::make_shared<RackChain>();
	chain->SetName(name);
	InsertChain((int)mChains.size(), chain);
	return chain;
}

void RackProcessor::InsertChain(int index, std::shared_ptr<RackChain> chain) {
	if (index < 0)
		index = 0;
	if (index > (int)mChains.size())
		index = (int)mChains.size();
	mChains.insert(mChains.begin() + index, std::move(chain));
	ProcessorHost::BumpChainGeneration();
}

void RackProcessor::RemoveChain(int index) {
	if (index < 0 || index >= (int)mChains.size())
		return;
	mChains.erase(mChains.begin() + index);
	ProcessorHost::BumpChainGeneration();
}

// ---- macros ----

void RackProcessor::SetVisibleMacroCount(int count) {
	mVisibleMacroCount = std::clamp(count, 1, kMaxMacros);
}

Parameter* RackProcessor::GetMacroParameter(int index) const {
	if (index < 0 || index >= (int)mMacroParams.size())
		return nullptr;
	return mMacroParams[index];
}

void RackProcessor::MapMacro(int macroIndex, const std::shared_ptr<AudioProcessor>& device,
							 const std::string& paramName, float minValue, float maxValue) {
	if (macroIndex < 0 || macroIndex >= kMaxMacros || !device)
		return;

	for (auto& mapping : mMacros[macroIndex].mappings) {
		if (mapping.device.lock() == device && mapping.paramName == paramName) {
			mapping.minValue = minValue;
			mapping.maxValue = maxValue;
			mBoundGeneration = 0; // force a re-resolve on the next block
			return;
		}
	}

	MacroMapping mapping;
	mapping.device = device;
	mapping.paramName = paramName;
	mapping.minValue = minValue;
	mapping.maxValue = maxValue;
	mMacros[macroIndex].mappings.push_back(std::move(mapping));
	mBoundGeneration = 0;
}

void RackProcessor::UnmapMacro(int macroIndex, int mappingIndex) {
	if (macroIndex < 0 || macroIndex >= kMaxMacros)
		return;
	auto& mappings = mMacros[macroIndex].mappings;
	if (mappingIndex < 0 || mappingIndex >= (int)mappings.size())
		return;
	mappings.erase(mappings.begin() + mappingIndex);
	mBoundGeneration = 0;
}

bool RackProcessor::IsParameterMapped(const Parameter* parameter) {
	if (!parameter)
		return false;
	RefreshMacroBindings();
	for (const auto& macro : mMacros) {
		for (const auto& mapping : macro.mappings) {
			if (mapping.resolved == parameter)
				return true;
		}
	}
	return false;
}

std::shared_ptr<AudioProcessor> RackProcessor::FindDeviceOwning(const Parameter* parameter) const {
	if (!parameter)
		return nullptr;

	std::shared_ptr<AudioProcessor> owner = nullptr;
	VisitDevices(mChains, [&](const std::shared_ptr<AudioProcessor>& device) {
		for (const auto& candidate : device->GetParameters()) {
			if (candidate.get() == parameter) {
				owner = device;
				return true;
			}
		}
		return false;
	});
	return owner;
}

void RackProcessor::RefreshMacroBindings() {
	const uint32_t generation = ProcessorHost::ChainGeneration();
	if (generation == mBoundGeneration)
		return;
	mBoundGeneration = generation;

	for (auto& macro : mMacros) {
		for (auto& mapping : macro.mappings) {
			mapping.resolved = nullptr;

			auto device = mapping.device.lock();
			if (!device)
				continue;

			// a device that has been dragged out of the rack (or deleted, and is only
			// still alive because the undo history holds it) must stop being driven
			bool insideRack = VisitDevices(mChains, [&](const std::shared_ptr<AudioProcessor>& candidate) {
				return candidate == device;
			});
			if (!insideRack)
				continue;

			for (const auto& parameter : device->GetParameters()) {
				if (parameter->name == mapping.paramName) {
					mapping.resolved = parameter.get();
					break;
				}
			}
		}
	}
}

void RackProcessor::ApplyMacros() {
	for (int i = 0; i < kMaxMacros; ++i) {
		const Parameter* macroParam = mMacroParams[i];
		const float t = std::clamp(macroParam->value / kMacroMax, 0.0f, 1.0f);

		for (const auto& mapping : mMacros[i].mappings) {
			if (!mapping.resolved)
				continue;
			// min > max is a deliberate inverted mapping, so the span is signed
			const float mapped = mapping.minValue + t * (mapping.maxValue - mapping.minValue);
			const float low = std::min(mapping.resolved->minValue, mapping.resolved->maxValue);
			const float high = std::max(mapping.resolved->minValue, mapping.resolved->maxValue);
			mapping.resolved->value = std::clamp(mapped, low, high);
		}
	}
}

int RackProcessor::GetSelectedChainIndex() const {
	if (mChains.empty())
		return -1;
	if (mSelectedChain < 0)
		return 0;
	if (mSelectedChain >= (int)mChains.size())
		return (int)mChains.size() - 1;
	return mSelectedChain;
}

std::shared_ptr<RackChain> RackProcessor::GetSelectedChain() {
	const int index = GetSelectedChainIndex();
	return index < 0 ? nullptr : mChains[index];
}

// ---- undo support ----

RackProcessor::RackState RackProcessor::CaptureState() const {
	RackState state;
	state.name = mName;
	state.color = mColor;
	state.visibleMacroCount = mVisibleMacroCount;
	state.macros = mMacros;
	state.chains = mChains;
	return state;
}

void RackProcessor::ApplyState(const RackState& state) {
	mName = state.name;
	mColor = state.color;
	mVisibleMacroCount = state.visibleMacroCount;
	mMacros = state.macros;
	mChains = state.chains;
	// the chain list and every mapping in it just changed wholesale
	ProcessorHost::BumpChainGeneration();
	mBoundGeneration = 0;
}

void RackProcessor::CopyStateFrom(const AudioProcessor& other) {
	const auto* source = dynamic_cast<const RackProcessor*>(&other);
	if (!source)
		return;

	mName = source->mName;
	mColor = source->mColor;
	mVisibleMacroCount = source->mVisibleMacroCount;
	mShowMacros = source->mShowMacros;
	mShowChains = source->mShowChains;
	mShowDevices = source->mShowDevices;
	mSelectedChain = source->mSelectedChain;

	// a copied rack is a copy all the way down: its chains are rebuilt from the
	// source's, and the macro mappings are re-pointed at the copies rather than left
	// aimed at the devices of the rack that was copied
	mChains.clear();
	for (const auto& sourceChain : source->mChains) {
		auto chain = std::make_shared<RackChain>();
		chain->SetName(sourceChain->GetName());
		chain->SetColor(sourceChain->GetColor());
		chain->SetMute(sourceChain->GetMute());
		chain->SetSolo(sourceChain->GetSolo());
		chain->GetVolumeParameter()->value = sourceChain->GetVolumeParameter()->value;
		chain->GetPanParameter()->value = sourceChain->GetPanParameter()->value;
		for (const auto& device : sourceChain->GetProcessors()) {
			if (auto clone = ProcessorIO::CloneProcessor(device))
				chain->AddProcessor(clone);
		}
		mChains.push_back(chain);
	}

	mMacros = source->mMacros;
	for (auto& macro : mMacros) {
		for (auto& mapping : macro.mappings) {
			// the source's path is the copy's path: the chains were rebuilt in order
			const std::string path = source->PathOfDevice(mapping.device.lock());
			mapping.device = DeviceAtPath(path);
			mapping.resolved = nullptr;
		}
	}

	ProcessorHost::BumpChainGeneration();
	mBoundGeneration = 0;
}

// ---- device paths ----

std::string RackProcessor::PathOfDevice(const std::shared_ptr<AudioProcessor>& device) const {
	if (!device)
		return "";

	for (size_t c = 0; c < mChains.size(); ++c) {
		auto& devices = mChains[c]->GetProcessors();
		for (size_t d = 0; d < devices.size(); ++d) {
			const std::string here = std::to_string(c) + "." + std::to_string(d);
			if (devices[d] == device)
				return here;
			if (auto* nested = dynamic_cast<RackProcessor*>(devices[d].get())) {
				const std::string deeper = nested->PathOfDevice(device);
				if (!deeper.empty())
					return here + "." + deeper;
			}
		}
	}
	return "";
}

std::shared_ptr<AudioProcessor> RackProcessor::DeviceAtPath(const std::string& path) const {
	std::vector<int> indices;
	std::stringstream ss(path);
	std::string part;
	while (std::getline(ss, part, '.')) {
		if (part.empty())
			return nullptr;
		indices.push_back(std::atoi(part.c_str()));
	}
	// a path is chain/device pairs all the way down
	if (indices.empty() || indices.size() % 2 != 0)
		return nullptr;

	const RackProcessor* rack = this;
	std::shared_ptr<AudioProcessor> device = nullptr;
	for (size_t i = 0; i < indices.size(); i += 2) {
		if (!rack)
			return nullptr;
		const int chainIndex = indices[i];
		const int deviceIndex = indices[i + 1];
		if (chainIndex < 0 || chainIndex >= (int)rack->mChains.size())
			return nullptr;
		auto& devices = rack->mChains[chainIndex]->GetProcessors();
		if (deviceIndex < 0 || deviceIndex >= (int)devices.size())
			return nullptr;
		device = devices[deviceIndex];
		rack = dynamic_cast<RackProcessor*>(device.get());
	}
	return device;
}

// ---- serialization ----

void RackProcessor::Save(std::ostream& out) {
	// everything of the rack's own goes ahead of the inherited PARAMS block:
	// AudioProcessor::Load stops at PARAMS_END and the caller eats exactly one line
	// after it (PROCESSOR_END), so there is no room for extra lines on the far side
	out << "RACK_NAME \"" << mName << "\"\n";
	out << "RACK_COLOR " << mColor << "\n";
	out << "RACK_MACRO_COUNT " << mVisibleMacroCount << "\n";
	out << "RACK_VIEW " << (mShowMacros ? 1 : 0) << " " << (mShowChains ? 1 : 0) << " "
		<< (mShowDevices ? 1 : 0) << " " << mSelectedChain << "\n";

	for (int i = 0; i < kMaxMacros; ++i) {
		const Macro& macro = mMacros[i];
		if (macro.title.empty() && macro.color == 0 && macro.mappings.empty())
			continue;
		out << "MACRO " << i << " \"" << macro.title << "\" " << macro.color << "\n";
		for (const auto& mapping : macro.mappings) {
			const std::string path = PathOfDevice(mapping.device.lock());
			if (path.empty())
				continue; // the device it named has left the rack
			out << "MMAP " << i << " \"" << path << "\" \"" << mapping.paramName << "\" "
				<< mapping.minValue << " " << mapping.maxValue << "\n";
		}
	}

	for (auto& chain : mChains)
		chain->Save(out);

	AudioProcessor::Save(out);
}

void RackProcessor::Load(std::istream& in) {
	// a mapping names its device by a path into chains that have not been read yet, so
	// the whole set is resolved once the body is in
	struct PendingMapping {
		int macroIndex;
		std::string path;
		std::string paramName;
		float minValue;
		float maxValue;
	};
	std::vector<PendingMapping> pending;

	mChains.clear();

	std::string line;
	while (std::getline(in, line)) {
		std::stringstream ss(line);
		std::string token;
		ss >> token;

		if (token == "RACK_NAME") {
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (q1 != std::string::npos && q2 != std::string::npos)
				mName = line.substr(q1 + 1, q2 - q1 - 1);
		} else if (token == "RACK_COLOR") {
			ss >> mColor;
		} else if (token == "RACK_MACRO_COUNT") {
			int count = kDefaultMacroCount;
			ss >> count;
			SetVisibleMacroCount(count);
		} else if (token == "RACK_VIEW") {
			int macros = 1, chains = 1, devices = 1;
			ss >> macros >> chains >> devices >> mSelectedChain;
			mShowMacros = (macros != 0);
			mShowChains = (chains != 0);
			mShowDevices = (devices != 0);
		} else if (token == "MACRO") {
			int index = -1;
			ss >> index;
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			if (index >= 0 && index < kMaxMacros && q1 != std::string::npos && q2 != std::string::npos) {
				mMacros[index].title = line.substr(q1 + 1, q2 - q1 - 1);
				std::stringstream rest(line.substr(q2 + 1));
				rest >> mMacros[index].color;
			}
		} else if (token == "MMAP") {
			int index = -1;
			ss >> index;
			size_t q1 = line.find('"');
			size_t q2 = line.find('"', q1 + 1);
			size_t q3 = line.find('"', q2 + 1);
			size_t q4 = line.find('"', q3 + 1);
			if (index >= 0 && index < kMaxMacros && q4 != std::string::npos) {
				PendingMapping mapping;
				mapping.macroIndex = index;
				mapping.path = line.substr(q1 + 1, q2 - q1 - 1);
				mapping.paramName = line.substr(q3 + 1, q4 - q3 - 1);
				mapping.minValue = 0.0f;
				mapping.maxValue = 1.0f;
				std::stringstream rest(line.substr(q4 + 1));
				rest >> mapping.minValue >> mapping.maxValue;
				pending.push_back(std::move(mapping));
			}
		} else if (token == "CHAIN_BEGIN") {
			auto chain = std::make_shared<RackChain>();
			chain->Load(in);
			mChains.push_back(std::move(chain));
		} else if (token == "PARAMS_BEGIN") {
			AudioProcessor::Load(in);
			break;
		}
	}

	for (const auto& entry : pending) {
		auto device = DeviceAtPath(entry.path);
		if (!device)
			continue;
		MapMacro(entry.macroIndex, device, entry.paramName, entry.minValue, entry.maxValue);
	}

	ProcessorHost::BumpChainGeneration();
	mBoundGeneration = 0;
}
