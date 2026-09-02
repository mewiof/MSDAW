#pragma once
#include <filesystem>
#include <string>

// paths as text, in one encoding: UTF-8
//
// NOTE: this exists because std::filesystem::path::string() THROWS on Windows for
// any name the active ANSI code page cannot represent - a Cyrillic, CJK or emoji
// filename is enough. On a background thread that is not an error to handle, it is
// std::terminate: the process is gone. u8string() never fails, so every conversion
// of a path that came off the filesystem goes through here
//
// the narrow strings the app passes around are therefore UTF-8, which is also what
// SDL hands us for a dropped file and what ImGui expects to draw. Anything that
// opens one converts it back to a path with ToPath rather than handing the bytes to
// a narrow fopen/ifstream, which would read them as ANSI
namespace PathText {

	inline std::string FromPath(const std::filesystem::path& path) {
		const std::u8string utf8 = path.u8string();
		return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
	}

	inline std::filesystem::path ToPath(const std::string& utf8) {
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
	}

} // namespace PathText
