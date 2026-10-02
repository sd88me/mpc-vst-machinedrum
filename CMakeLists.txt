cmake_minimum_required(VERSION 3.22)
project(MpcVstMachinedrum LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

# MD_DISCOVERY: build only the recompiler's discovery tool (tools/mdrecomp): the DSP emulator has to be built with its trace
# hook and on the plain interpreter for it, so this is a separate build directory (release/build_release.sh does it).
option(MD_DISCOVERY "build mdrecomp-discover (the static recompiler's tracer) instead of the normal targets" OFF)
if(MD_DISCOVERY)
  string(APPEND CMAKE_CXX_FLAGS " -DDSP56K_RECOMP_DISCOVERY -DDSP56K_NO_JIT_RUNTIME")
endif()

set(MPC_VST_DIR "" CACHE PATH "mpc-vst-plugins checkout (the shared VST2 wrapper); only needed for the plugin .so target")

include(cmake/dsp56300.cmake)

add_library(mdcore STATIC
  engine/HostModel.cpp
  engine/MachineRunner.cpp
  engine/Mixer.cpp
  engine/ParallelVoiceEngine.cpp
  engine/TrackFx.cpp
  engine/VoiceEngine.cpp
  tools/mdfw/Firmware.cpp
)
target_include_directories(mdcore PUBLIC engine tools/mdfw)
target_include_directories(mdcore PRIVATE
  libs/gearmulator-md-mm/source libs/gearmulator-md-mm/source/mc68k)
target_link_libraries(mdcore PUBLIC dsp56kEmu 68kEmu)

# mdmachine: lists the OS's machine table (names, SYN labels) - the skin generator's input (tools/mdskin)
add_executable(mdmachine tools/mdmachine/mdmachine.cpp)
target_link_libraries(mdmachine PRIVATE mdcore pthread)
if(MD_DISCOVERY)
  add_executable(mdrecomp-discover tools/mdrecomp/mdrecomp_discover.cpp)
  target_link_libraries(mdrecomp-discover PRIVATE mdcore pthread dl)
endif()

# mdartdump: the LCD fonts and dial art of the user's own OS as JSON, for the skin generator (tools/mdskin)
add_executable(mdartdump tools/mdskin/mdartdump.cpp)
target_link_libraries(mdartdump PRIVATE mdcore)

# mdcost: per-machine cost of one playing voice (instructions and wall time), for the Force
add_executable(md-cost tools/mdcost/mdcost.cpp)
target_link_libraries(md-cost PRIVATE mdcore pthread dl)
# mdhash: an audio hash of a fixed pattern, to compare builds
add_executable(md-hash tools/mdhash/mdhash.cpp)
target_link_libraries(md-hash PRIVATE mdcore pthread dl)
# mdsamples: extracts the ROM machines' sample memory from a dump of the emulated MD's voice DSP (tools/mdkits)
add_executable(mdsamples tools/mdkits/mdsamples.cpp)
target_link_libraries(mdsamples PRIVATE mdcore pthread)

# machinedrum_one: the same engine source (vst/engine.cpp) built against the shared wrapper
# (mpc-vst-plugins' vst2_wrap.c). Needs its generated params.h (mpc-vst's gen_vst.py --params-h, run by
# vst/build_so.sh) on its include path first (vst/build/).
if(MPC_VST_DIR)
  add_library(machinedrum_one SHARED vst/engine.cpp "${MPC_VST_DIR}/wrapper/vst2_wrap.c")
  target_include_directories(machinedrum_one PRIVATE "${MPC_VST_DIR}/wrapper" "${CMAKE_CURRENT_SOURCE_DIR}/vst/build")
  target_link_libraries(machinedrum_one PRIVATE mdcore pthread)
  set_target_properties(machinedrum_one PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
  target_link_options(machinedrum_one PRIVATE -Wl,--no-undefined,--exclude-libs,ALL,-Bsymbolic -static-libstdc++ -static-libgcc)

  # machinedrum_tap: one track or send of the Machinemodule in the same process, mono, for MPC's own mixer (vst/tap)
  add_library(machinedrum_tap SHARED vst/tap/tap_engine.cpp "${MPC_VST_DIR}/wrapper/vst2_wrap.c")
  target_include_directories(machinedrum_tap PRIVATE "${MPC_VST_DIR}/wrapper" "${CMAKE_CURRENT_SOURCE_DIR}/vst/tap/build" "${CMAKE_CURRENT_SOURCE_DIR}/vst")
  target_link_libraries(machinedrum_tap PRIVATE dl)
  set_target_properties(machinedrum_tap PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
  target_link_options(machinedrum_tap PRIVATE -Wl,--no-undefined,--exclude-libs,ALL,-Bsymbolic -static-libstdc++ -static-libgcc)

  # machinedrum_tapfx: the same tap as an audio effect for an MPC return/FX track (vst/tapfx): outputs the tapped source
  add_library(machinedrum_tapfx SHARED vst/tap/tap_engine.cpp "${MPC_VST_DIR}/wrapper/vst2_wrap.c")
  target_compile_definitions(machinedrum_tapfx PRIVATE TAP_FX=1)
  target_include_directories(machinedrum_tapfx PRIVATE "${MPC_VST_DIR}/wrapper" "${CMAKE_CURRENT_SOURCE_DIR}/vst/tapfx/build" "${CMAKE_CURRENT_SOURCE_DIR}/vst")
  target_link_libraries(machinedrum_tapfx PRIVATE dl)
  set_target_properties(machinedrum_tapfx PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
  target_link_options(machinedrum_tapfx PRIVATE -Wl,--no-undefined,--exclude-libs,ALL,-Bsymbolic -static-libstdc++ -static-libgcc)

  add_executable(md-vst-smoke vst/smoke.cpp vst/engine.cpp)
  target_include_directories(md-vst-smoke PRIVATE "${MPC_VST_DIR}/wrapper" "${CMAKE_CURRENT_SOURCE_DIR}/vst/build")
  target_link_libraries(md-vst-smoke PRIVATE mdcore pthread)
endif()
