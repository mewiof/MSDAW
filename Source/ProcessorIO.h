#pragma once
#include <iosfwd>
#include <memory>
#include <string>

class AudioProcessor;

// ================================================================
// DEVICE CONSTRUCTION AND SERIALIZATION
// ================================================================
// one device on disk is a "PROCESSOR <id>" line, an optional PROC_SCALING line, the
// device's own body, and a PROCESSOR_END line. a track writes its chain with this, and
// so does a rack writing the chains nested inside it - the format is identical at every
// depth, which is what lets a rack be saved as just another device
namespace ProcessorIO {

	void SaveProcessor(std::ostream& out, AudioProcessor& processor);

	// reads the block whose "PROCESSOR <id>" line the caller has just consumed, up to
	// and including its PROCESSOR_END. an id this build has no device for skips the
	// block and returns null, rather than leaving the rest of it to be misread as
	// track content
	std::shared_ptr<AudioProcessor> LoadProcessor(std::istream& in, const std::string& processorId);

	// a second device of the same kind carrying the same state - what copy, paste and
	// duplicate build, and what a rack builds when it is itself copied. it is the same
	// question the loader answers ("make me this device again"), so it lives beside it
	// rather than inside whichever view happened to need it first. null when the source
	// is a plugin whose library will not load a second time
	std::shared_ptr<AudioProcessor> CloneProcessor(const std::shared_ptr<AudioProcessor>& source);

} // namespace ProcessorIO
