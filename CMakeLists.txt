cmake_minimum_required(VERSION 3.25.0)

project(MSDAW LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

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

# VST 2.4 SDK
set(VST2_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/External/vstsdk2.4")
file(GLOB VST2_SDK_SOURCE_FILES CONFIGURE_DEPENDS
	"${VST2_SDK_PATH}/public.sdk/source/vst2.x/audioeffect.cpp"
	"${VST2_SDK_PATH}/public.sdk/source/vst2.x/audioeffectx.cpp"
)

# local
set(MSDAW_SOURCE_PATH "${CMAKE_SOURCE_DIR}/Source")
file(GLOB_RECURSE MSDAW_SOURCE_FILES CONFIGURE_DEPENDS "${MSDAW_SOURCE_PATH}/*.cpp")

set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

# workaround for VST2 SDK legacy code issues
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
	# -Wno-writable-strings: Allow conversion from string literal to 'char *'
	# -Wno-c++11-narrowing: Allow narrowing conversions in initialization lists (e.g. int to char)
	set_source_files_properties(${VST2_SDK_SOURCE_FILES} PROPERTIES COMPILE_OPTIONS "-Wno-writable-strings;-Wno-c++11-narrowing")
endif()

add_executable(MSDAW
	# external
	${IMGUI_SOURCE_FILES}
	${VST2_SDK_SOURCE_FILES}
	# local
	${MSDAW_SOURCE_FILES}
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
	# VST3 SDK uses #pragma pack which triggers warnings in Clang/GCC, disable them for MSDAW
	target_compile_options(MSDAW PRIVATE "-Wno-pragma-pack")
endif()

target_include_directories(MSDAW
	PRIVATE
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

target_link_libraries(MSDAW
	PRIVATE
		opengl32
		SDL3::SDL3
		rtaudio
		Freetype::Freetype
)

target_compile_definitions(MSDAW
	PRIVATE
		_CRT_SECURE_NO_WARNINGS
)
target_precompile_headers(MSDAW
	PRIVATE
		${MSDAW_SOURCE_PATH}/PrecompHeader.h
)
