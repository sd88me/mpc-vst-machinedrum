# Machinemodule for the desktop (VST3 / AU / Standalone): the Machinedrum engine of the parent repo inside a JUCE plugin,
# framework after shnolk's Monomodule. Build:  cmake -S desktop -B desktop/build -G Ninja && cmake --build desktop/build
# Needs git submodules (git submodule update --init --recursive) and an internet connection for the first configure (JUCE).
cmake_minimum_required(VERSION 3.22)
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
endif()
project(MachinemoduleDesktop VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
  add_compile_options(/utf-8 /bigobj)
  set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

get_filename_component(MD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# the emulators (dsp56300 JIT build, Musashi) shared with the MPC build
include("${MD_ROOT}/cmake/dsp56300.cmake")

# the engine: the same sources as the parent repo's mdcore
add_library(mdcore STATIC
  ${MD_ROOT}/engine/HostModel.cpp
  ${MD_ROOT}/engine/MachineRunner.cpp
  ${MD_ROOT}/engine/Mixer.cpp
  ${MD_ROOT}/engine/ParallelVoiceEngine.cpp
  ${MD_ROOT}/engine/TrackFx.cpp
  ${MD_ROOT}/engine/VoiceEngine.cpp
  ${MD_ROOT}/tools/mdfw/Firmware.cpp
)
target_include_directories(mdcore PUBLIC ${MD_ROOT}/engine ${MD_ROOT}/tools/mdfw)
target_include_directories(mdcore PRIVATE ${MD_ROOT}/libs/gearmulator-md-mm/source ${MD_ROOT}/libs/gearmulator-md-mm/source/mc68k)
target_link_libraries(mdcore PUBLIC dsp56kEmu 68kEmu)
if(NOT MSVC)
  target_compile_options(mdcore PRIVATE -w)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.9
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

if(WIN32)
  option(MM_COPY_PLUGINS "Copy the plugins to the system plugin folders after building" OFF)
else()
  option(MM_COPY_PLUGINS "Copy the plugins to the user's plugin folders after building" OFF)
endif()
set(MM_HARDENED_OPTS com.apple.security.cs.allow-jit com.apple.security.cs.allow-unsigned-executable-memory com.apple.security.cs.disable-library-validation)

set(MM_SOURCES
  src/core/Resampler.cpp
  src/plugin/MdProcessor.cpp
  src/plugin/MdEditor.cpp)

juce_add_plugin(Machinemodule
  PRODUCT_NAME "Machinemodule"
  COMPANY_NAME "sd88me"
  BUNDLE_ID "io.github.sd88me.machinemodule"
  PLUGIN_MANUFACTURER_CODE Sd88
  PLUGIN_CODE Mdrm
  FORMATS VST3 AU Standalone
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD ${MM_COPY_PLUGINS}
  AU_MAIN_TYPE kAudioUnitType_MusicDevice
  HARDENED_RUNTIME_ENABLED TRUE
  HARDENED_RUNTIME_OPTIONS ${MM_HARDENED_OPTS}
  MICROPHONE_PERMISSION_ENABLED FALSE)
target_sources(Machinemodule PRIVATE ${MM_SOURCES})
target_include_directories(Machinemodule PRIVATE src/core src/plugin)
target_compile_definitions(Machinemodule PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(Machinemodule
  PRIVATE mdcore juce::juce_audio_utils
  PUBLIC juce::juce_recommended_config_flags)

# Offline renderer / test: notes through the processor to a WAV, compared with the bare engine (tests/mm_render.cpp)
juce_add_console_app(MmRender PRODUCT_NAME "mm-render")
target_sources(MmRender PRIVATE tests/mm_render.cpp ${MM_SOURCES})
target_include_directories(MmRender PRIVATE src/core src/plugin)
target_compile_definitions(MmRender PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=0
  JucePlugin_Name="Machinemodule" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(MmRender PRIVATE mdcore juce::juce_audio_utils juce::juce_recommended_config_flags)
