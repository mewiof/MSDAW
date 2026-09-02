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

		// optional per-plugin editor scaling override (written since the high-DPI work;
		// older projects omit it and rewind untouched)
		std::streampos posBefore = in.tellg();
		std::string maybeScaling;
		if (std::getline(in, maybeScaling)) {
			std::stringstream ss(maybeScaling);
			std::string token;
			ss >> token;
			if (token == "PROC_SCALING") {
				int mode = 0;
				ss >> mode;
				processor->SetEditorScalingMode((EditorScalingMode)mode);
			} else if (posBefore != std::streampos(-1)) {
				in.seekg(posBefore); // not ours; let the device consume it
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
		clone->CopyStateFrom(*source); // anything the parameter list does not cover

		return clone;
	}

} // namespace ProcessorIO
