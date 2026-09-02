cmake_minimum_required(VERSION 3.25.0)

project(MSDAW LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(MSDAW_BUILD_TESTS "Build the MSDAW test suite" ON)

# external
set(SUBMODULES_PATH "${CMAKE_CURRENT_SOURCE_DIR}/External/Submodules")

# ---- STATIC LINKING ----
# 1. force standard CMake projects to build static libs
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)

# 2. SDL3 specifics: disable shared, enable static, disable tests/examples
set(SDL_SHARED OFF CACHE BOOL "Build shared SDL lib" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build static SDL lib" FORCE)
set(SDL_TEST OFF CACHE BOOL "Build SDL test" FORCE)

# 3. RtAudio specifics: disable shared, enable static
set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared RtAudio" FORCE)
set(RTAUDIO_BUILD_STATIC_LIBS ON CACHE BOOL "Build static RtAudio" FORCE)
# ------------------------------------

add_subdirectory("${SUBMODULES_PATH}/SDL")

file(GLOB IMGUI_SOURCE_FILES CONFIGURE_DEPENDS
	"${SUBMODULES_PATH}/imgui/*.cpp"
	"${SUBMODULES_PATH}/imgui/backends/imgui_impl_opengl3.cpp"
	"${SUBMODULES_PATH}/imgui/backends/imgui_impl_sdl3.cpp"
	"${SUBMODULES_PATH}/imgui/misc/freetype/imgui_freetype.cpp"
)

add_subdirectory("${SUBMODULES_PATH}/rtaudio")
add_subdirectory("${SUBMODULES_PATH}/freetype")
add_library(Freetype::Freetype ALIAS freetype)

# NOTE: googletest is added before CMAKE_COMPILE_WARNING_AS_ERROR is turned on,
# like every other submodule -- its warnings are not ours to fix
if(MSDAW_BUILD_TESTS)
	# match Clang's default dynamic CRT on Windows; a mismatch surfaces as link errors
	set(gtest_force_shared_crt ON CACHE BOOL "Use shared (DLL) run-time lib" FORCE)
	set(INSTALL_GTEST OFF CACHE BOOL "Install googletest" FORCE)
	add_subdirectory("${SUBMODULES_PATH}/googletest")

	enable_testing()
	include(GoogleTest)
endif()

# ================================================================
# THIRD-PARTY NOTICES
# ================================================================

# MSDAW ships as a single exe with no files beside it, so every notice its
# dependencies ask a binary distribution to carry has to live inside the binary
# itself. each entry below reads that project's own license file at configure time
# and the window in the app draws what comes back verbatim - nothing is
# transcribed by hand, so the credits cannot drift from what is actually linked in
set(MSDAW_NOTICE_ROWS "")
set(MSDAW_NOTICE_FILES "")

# NOTE: a macro rather than a function so the appends land in this scope.
# an empty licensePath means we are in no position to redistribute the text, and
# the window says so instead of showing a license we were never granted
macro(msdaw_add_notice componentName componentURL componentRole licenseName extraNotice licensePath)
	set(MSDAW_NOTICE_TEXT "")
	if(NOT "${licensePath}" STREQUAL "")
		if(NOT EXISTS "${licensePath}")
			message(FATAL_ERROR "MSDAW: no license file for ${componentName} at ${licensePath} - run 'git submodule update --init --recursive'")
		endif()
		file(READ "${licensePath}" MSDAW_NOTICE_TEXT)
		# the license files are CRLF and imgui draws a stray CR as a missing glyph
		string(REPLACE "\r\n" "\n" MSDAW_NOTICE_TEXT "${MSDAW_NOTICE_TEXT}")
		list(APPEND MSDAW_NOTICE_FILES "${licensePath}")
	endif()
	string(APPEND MSDAW_NOTICE_ROWS
		"\t{\"${componentName}\", \"${componentURL}\", \"${componentRole}\", \"${licenseName}\", \"${extraNotice}\",\n"
		"\t R\"MSDAW_NOTICE(${MSDAW_NOTICE_TEXT})MSDAW_NOTICE\"},\n\n")
endmacro()

msdaw_add_notice("MSDAW" "https://github.com/mewiof/MSDAW"
	"this application" "MIT License" ""
	"${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

msdaw_add_notice("SDL3" "https://github.com/libsdl-org/SDL"
	"platform window management & input" "zlib License" ""
	"${SUBMODULES_PATH}/SDL/LICENSE.txt")

msdaw_add_notice("Dear ImGui" "https://github.com/ocornut/imgui"
	"user interface" "MIT License" ""
	"${SUBMODULES_PATH}/imgui/LICENSE.txt")

msdaw_add_notice("RtAudio" "https://github.com/thestk/rtaudio"
	"audio I/O abstraction" "MIT License" ""
	"${SUBMODULES_PATH}/rtaudio/LICENSE")

# NOTE: freetype is dual licensed and MSDAW takes the FTL arm, so docs/FTL.TXT is
# the text that applies rather than the GPLv2 one beside it. FTL section 2 asks a
# binary distribution to state in its documentation that the software is based in
# part on the FreeType Team's work - a single-exe build has no documentation other
# than this window and the readme, so the sentence is carried in both
msdaw_add_notice("FreeType" "https://freetype.org"
	"font engine" "FreeType License (FTL)"
	"MSDAW is based in part on the work of the FreeType Team."
	"${SUBMODULES_PATH}/freetype/docs/FTL.TXT")

# NOTE: MIT as of SDK 3.8.0. steinberg relicensed, so this is no longer the GPLv3
# arm of a dual license, and nothing in it reaches MSDAW's own terms
msdaw_add_notice("VST3 SDK" "https://github.com/steinbergmedia/vst3sdk"
	"VST3 plugin hosting" "MIT License" ""
	"${SUBMODULES_PATH}/vst3sdk/LICENSE.txt")

# NOTE: no text to embed. steinberg withdrew VST2 licensing in 2018 and the
# agreement it shipped under never let us redistribute it, so quoting it here
# would be claiming a grant we do not have
msdaw_add_notice("VST 2.4 SDK" ""
	"VST2 plugin interface logic" "Proprietary/Legacy Steinberg"
	"Contact mewiof@gmail.com for instant removal."
	"")

msdaw_add_notice("googletest" "https://github.com/google/googletest"
	"test suite only, never linked into the app" "BSD 3-Clause License" ""
	"${SUBMODULES_PATH}/googletest/LICENSE")

set(MSDAW_GENERATED_PATH "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(MSDAW_NOTICE_HEADER "// generated by CMake from each dependency's own license file - do not edit
// see the THIRD-PARTY NOTICES section of CMakeLists.txt
#pragma once

// one dependency as the third-party window lists it. text is that project's
// license verbatim; it is empty where we hold no right to redistribute it
struct ThirdPartyNotice {
	const char* name;
	const char* uRL;
	const char* role;
	const char* license;
	const char* notice; // a statement that project's terms ask us to carry, usually empty
	const char* text;
};

static const ThirdPartyNotice kThirdPartyNotices[] = {
${MSDAW_NOTICE_ROWS}};
")

# a dependency that changes its license text has to reach the binary, so touching
# one re-runs configure and regenerates the header
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MSDAW_NOTICE_FILES})

# COPYONLY leaves the header's timestamp alone when the content has not changed,
# so an ordinary reconfigure does not force Editor.cpp to rebuild
file(WRITE "${MSDAW_GENERATED_PATH}/ThirdPartyNotices.h.in" "${MSDAW_NOTICE_HEADER}")
configure_file("${MSDAW_GENERATED_PATH}/ThirdPartyNotices.h.in" "${MSDAW_GENERATED_PATH}/ThirdPartyNotices.h" COPYONLY)

# VST 2.4 SDK
set(VST2_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/External/vstsdk2.4")
file(GLOB VST2_SDK_SOURCE_FILES CONFIGURE_DEPENDS
	"${VST2_SDK_PATH}/public.sdk/source/vst2.x/audioeffect.cpp"
	"${VST2_SDK_PATH}/public.sdk/source/vst2.x/audioeffectx.cpp"
)

# local
set(MSDAW_SOURCE_PATH "${CMAKE_SOURCE_DIR}/Source")
file(GLOB_RECURSE MSDAW_SOURCE_FILES CONFIGURE_DEPENDS "${MSDAW_SOURCE_PATH}/*.cpp")

# Main.cpp owns the window/audio bootstrap, so it stays out of the core and the
# test binary can supply its own entry point
list(REMOVE_ITEM MSDAW_SOURCE_FILES "${MSDAW_SOURCE_PATH}/Main.cpp")

set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

# workaround for VST2 SDK legacy code issues
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
	# -Wno-writable-strings: Allow conversion from string literal to 'char *'
	# -Wno-c++11-narrowing: Allow narrowing conversions in initialization lists (e.g. int to char)
	set_source_files_properties(${VST2_SDK_SOURCE_FILES} PROPERTIES COMPILE_OPTIONS "-Wno-writable-strings;-Wno-c++11-narrowing")
endif()

# ================================================================
# MSDAWCORE
# ================================================================

# NOTE: OBJECT, never STATIC -- built-in processors self-register through a
# file-scope ProcessorRegistrar (see REGISTER_PROCESSOR) that nothing
# references by symbol. A static archive lets the linker drop those objects
# and the factory silently comes up empty, in the app and in the tests alike
add_library(MSDAWCore OBJECT
	# external
	${IMGUI_SOURCE_FILES}
	${VST2_SDK_SOURCE_FILES}
	# local
	${MSDAW_SOURCE_FILES}
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
	# VST3 SDK uses #pragma pack which triggers warnings in Clang/GCC, disable them for MSDAW
	target_compile_options(MSDAWCore PUBLIC "-Wno-pragma-pack")
endif()

target_include_directories(MSDAWCore
	PUBLIC
		${MSDAW_SOURCE_PATH}
		${MSDAW_GENERATED_PATH}
	SYSTEM
		${SUBMODULES_PATH}/SDL/include
		${SUBMODULES_PATH}/imgui
		${SUBMODULES_PATH}/rtaudio/include
		${SUBMODULES_PATH}/freetype/include
		${SUBMODULES_PATH}/vst3sdk
		${VST2_SDK_PATH}
		${VST2_SDK_PATH}/public.sdk/source/vst2.x
)

target_link_libraries(MSDAWCore
	PUBLIC
		opengl32
		SDL3::SDL3
		rtaudio
		Freetype::Freetype
)

# the library explorer's folder picker (SHBrowseForFolder) and "show in explorer"
# (ShellExecute) live in shell32; the picker frees its result with CoTaskMemFree
if(WIN32)
	target_link_libraries(MSDAWCore PUBLIC shell32 ole32)
endif()

target_compile_definitions(MSDAWCore
	PUBLIC
		_CRT_SECURE_NO_WARNINGS
)
target_precompile_headers(MSDAWCore
	PUBLIC
		${MSDAW_SOURCE_PATH}/PrecompHeader.h
)

# ================================================================
# MSDAW
# ================================================================

add_executable(MSDAW ${MSDAW_SOURCE_PATH}/Main.cpp)

target_link_libraries(MSDAW PRIVATE MSDAWCore)

# ================================================================
# MSDAWTESTS
# ================================================================

if(MSDAW_BUILD_TESTS)
	set(MSDAW_TEST_PATH "${CMAKE_SOURCE_DIR}/Tests")
	file(GLOB_RECURSE MSDAW_TEST_FILES CONFIGURE_DEPENDS "${MSDAW_TEST_PATH}/*.cpp")

	add_executable(MSDAWTests ${MSDAW_TEST_FILES})

	target_link_libraries(MSDAWTests
		PRIVATE
			MSDAWCore
			GTest::gmock
			GTest::gtest_main
	)

	# NOTE: DISCOVERY_MODE PRE_TEST defers enumeration to test time; enumerating
	# at build time would run the binary on a machine that may have no audio device
	gtest_discover_tests(MSDAWTests DISCOVERY_MODE PRE_TEST)
endif()
