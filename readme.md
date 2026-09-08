https://github.com/user-attachments/assets/1edac651-7c0c-4511-9eb0-d284dfb59650

**MSDAW** (short for _Mewiof's Silly Digital Audio Workstation_) is a effortless/bug-filled experiment in building an Ableton/FL Studio-like app

## Features

- **plugin hosting:** support for VST2 and VST3 instruments/effects
- **built-in plugins:** Bit Crusher, Auto Sidechain, EQ Eight, Analyzer, Modulator (LFO / Performer / Stepper)
- **racks:** group devices into a rack of parallel chains, driven by mappable, automatable macro knobs
- **timeline engine:** audio/MIDI clips (with linking)
- **track management:** hierarchical track grouping and routing
- **automation:** parameter automation with curved tension
- **mixing:** per-track volume, panning, solo/mute, live peak metering
- **export:** offline rendering to 16-bit WAV files
- **UI:** hardware-accelerated interface
- **input:** computer MIDI keyboard

## Community

Chat with me: [https://pony.land/](https://pony.land/)

## Building

The project uses **CMake** & configured for Clang/Windows

Install [LLVM](https://github.com/llvm/llvm-project/releases) & [CMake](https://cmake.org/download/) (+ [Ninja](https://ninja-build.org/) optionally)

1. Clone with submodules: `git clone --recurse-submodules https://github.com/mewiof/MSDAW.git`
2. Open in Visual Studio/VSCode or use CMake:
    ```bash
    mkdir build
    cd build
    cmake ..
    ```

## Notices & Licenses

MSDAW is released under the [MIT License](LICENSE) and relies on:

- **[SDL3](https://github.com/libsdl-org/SDL):** platform window management & input (zlib License)
- **[Dear ImGui](https://github.com/ocornut/imgui):** user interface (MIT License)
- **[RtAudio](https://github.com/thestk/rtaudio):** audio I/O abstraction (MIT License)
- **[FreeType](https://freetype.org):** font engine (FreeType License, the FTL arm of its dual license)
- **[VST3 SDK](https://github.com/steinbergmedia/vst3sdk):** VST3 plugin hosting (MIT License, as of SDK 3.8.0)
- **VST 2.4 SDK:** VST2 plugin interface logic (Proprietary/Legacy Steinberg license; contact `mewiof@gmail.com` for instant removal)
- **[googletest](https://github.com/google/googletest):** test suite only, never linked into the app (BSD 3-Clause License)

MSDAW is based in part on the work of the FreeType Team.

MSDAW ships as a single executable, so it carries these licenses in full inside the
binary rather than in files beside it — **Third Party** in the menu bar shows each one.
The texts are read from every dependency's own license file when CMake configures the
build (see the *Third-party notices* section of [CMakeLists.txt](CMakeLists.txt)), so a
new dependency is credited by adding one line there, not by transcribing anything.
