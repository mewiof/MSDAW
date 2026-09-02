#include "PrecompHeader.h"
#include "ProcessorHost.h"

#include "AudioProcessor.h"

std::atomic<uint32_t> ProcessorHost::sChainGeneration{1};

void ProcessorHost::AddProcessor(std::shared_ptr<AudioProcessor> processor) {
	InsertProcessor((int)GetProcessors().size(), std::move(processor));
}

void ProcessorHost::InsertProcessor(int index, std::shared_ptr<AudioProcessor> processor) {
	auto& processors = GetProcessors();
	if (index < 0)
		index = 0;
	if (index > (int)processors.size())
		index = (int)processors.size();
	processors.insert(processors.begin() + index, std::move(processor));
	BumpChainGeneration();
}

void ProcessorHost::RemoveProcessor(int index) {
	auto& processors = GetProcessors();
	if (index >= 0 && index < (int)processors.size()) {
		processors.erase(processors.begin() + index);
		BumpChainGeneration();
	}
}

void ProcessorHost::MoveProcessor(int fromIndex, int toIndex) {
	auto& processors = GetProcessors();
	if (fromIndex < 0 || fromIndex >= (int)processors.size())
		return;
	if (toIndex < 0 || toIndex > (int)processors.size())
		return;
	auto processor = processors[fromIndex];
	if (toIndex > fromIndex)
		toIndex--;
	processors.erase(processors.begin() + fromIndex);
	processors.insert(processors.begin() + toIndex, processor);
	BumpChainGeneration();
}

void ProcessorHost::SetProcessors(std::vector<std::shared_ptr<AudioProcessor>> processors) {
	GetProcessors() = std::move(processors);
	BumpChainGeneration();
}

void ProcessorHost::CollectParameters(std::vector<Parameter*>& out) {
	CollectOwnParameters(out);
	for (auto& processor : GetProcessors()) {
		if (!processor)
			continue;
		for (auto& parameter : processor->GetParameters())
			out.push_back(parameter.get());

		std::vector<ProcessorHost*> nested;
		processor->CollectHostedChains(nested);
		for (ProcessorHost* chain : nested) {
			if (chain)
				chain->CollectParameters(out);
		}
	}
}
