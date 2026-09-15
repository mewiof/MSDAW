#include "PrecompHeader.h"
#include "ProcessorIO.h"

#include "AudioProcessor.h"
#include "ProcessorFactory.h"
#include "Processors/VSTProcessor.h"
#include "Processors/VST3Processor.h"

#include <istream>
#include <ostream>
#include <sstream>

namespace ProcessorIO {

	void SaveProcessor(std::ostream& out, AudioProcessor& processor) {
		out << "PROCESSOR " << processor.GetProcessorId() << "\n";
		out << "PROC_SCALING " << (int)processor.GetEditorScalingMode() << "\n";
		// only written once a panel has actually been configured, so a device that
		// shows everything keeps writing exactly what it used to
		const std::vector<int>& panel = processor.GetPanelParameters();
		if (!panel.empty()) {
			out << "PROC_PANEL";
			for (int index : panel)
				out << " " << index;
			out << "\n";
		}
		processor.Save(out);
		out << "PROCESSOR_END\n";
	}

	std::shared_ptr<AudioProcessor> LoadProcessor(std::istream& in, const std::string& processorId) {
		std::shared_ptr<AudioProcessor> processor = ProcessorFactory::Instance().Create(processorId);
		// the plugin wrappers are built from a path, not from the factory: the path is
		// the first thing in the body they are about to read
		if (!processor && processorId == "VST")
			processor = std::make_shared<VSTProcessor>("");
		else if (!processor && processorId == "VST3")
			processor = std::make_shared<VST3Processor>("", "");

		if (!processor) {
			std::string skipped;
			while (std::getline(in, skipped)) {
				if (skipped == "PROCESSOR_END")
					break;
			}
			return nullptr;
		}

		// optional PROC_ lines sit between the id and the device's own body, and each
		// was added after projects already existed - so any of them may be missing.
		// read them while they keep coming, and rewind the first line that is not ours
		// for the device to consume
		while (true) {
			std::streampos posBefore = in.tellg();
			std::string maybeProcLine;
			if (!std::getline(in, maybeProcLine))
				break;

			std::stringstream ss(maybeProcLine);
			std::string token;
			ss >> token;

			if (token == "PROC_SCALING") {
				int mode = 0;
				ss >> mode;
				processor->SetEditorScalingMode((EditorScalingMode)mode);
			} else if (token == "PROC_PANEL") {
				std::vector<int> panel;
				int index = 0;
				while (ss >> index)
					panel.push_back(index);
				processor->SetPanelParameters(std::move(panel));
			} else {
				if (posBefore != std::streampos(-1))
					in.seekg(posBefore);
				break;
			}
		}

		processor->Load(in);

		// a device stops reading at the end of its own body, so the block terminator is
		// still sitting there
		std::string endTag;
		std::getline(in, endTag);

		return processor;
	}

	std::shared_ptr<AudioProcessor> CloneProcessor(const std::shared_ptr<AudioProcessor>& source) {
		if (!source)
			return nullptr;

		std::shared_ptr<AudioProcessor> clone = nullptr;

		if (auto vST = std::dynamic_pointer_cast<VSTProcessor>(source)) {
			clone = std::make_shared<VSTProcessor>(vST->GetPath());
			if (!std::static_pointer_cast<VSTProcessor>(clone)->Load())
				return nullptr;
		} else if (auto vST3 = std::dynamic_pointer_cast<VST3Processor>(source)) {
			clone = std::make_shared<VST3Processor>(vST3->GetPath(), vST3->GetClassID());
			if (!std::static_pointer_cast<VST3Processor>(clone)->Load())
				return nullptr;
		} else {
			clone = ProcessorFactory::Instance().Create(source->GetProcessorId());
		}

		if (!clone)
			return nullptr;

		// two instances of the same class list their parameters in the same order
		const auto& sourceParams = source->GetParameters();
		const auto& cloneParams = clone->GetParameters();
		for (size_t i = 0; i < sourceParams.size() && i < cloneParams.size(); ++i)
			cloneParams[i]->value = sourceParams[i]->value;

		clone->SetBypassed(source->IsBypassed());
		clone->SetEditorScalingMode(source->GetEditorScalingMode());
		clone->SetPanelParameters(source->GetPanelParameters());
		clone->CopyStateFrom(*source); // anything the parameter list does not cover

		return clone;
	}

} // namespace ProcessorIO
