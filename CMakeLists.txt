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
