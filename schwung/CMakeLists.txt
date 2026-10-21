# Machinemodule for Schwung (Ableton Move): the module's two binaries, cross-compiled for aarch64 (schwung/scripts/build.sh).
#   dsp.so      the plugin half, loaded into MoveOriginal (no emulator in it: it copies audio and queues commands)
#   md-engine   the engine process, spawned per instance (the whole emulator; linked statically: Move has glibc 2.35)
cmake_minimum_required(VERSION 3.22)
project(MachinemoduleSchwung LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

set(MD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include(${MD_ROOT}/cmake/dsp56300.cmake)

# the engine: the same sources as the parent repo's mdcore
add_library(mdcore STATIC
  ${MD_ROOT}/engine/HostModel.cpp
  ${MD_ROOT}/engine/MachineRunner.cpp
  ${MD_ROOT}/engine/MasterFx.cpp
  ${MD_ROOT}/engine/Mixer.cpp
  ${MD_ROOT}/engine/ParallelVoiceEngine.cpp
  ${MD_ROOT}/engine/TrackFx.cpp
  ${MD_ROOT}/engine/VoiceEngine.cpp
  ${MD_ROOT}/tools/mdfw/Firmware.cpp)
target_include_directories(mdcore PUBLIC ${MD_ROOT}/engine ${MD_ROOT}/tools/mdfw)
target_include_directories(mdcore PRIVATE ${MD_ROOT}/libs/gearmulator-md-mm/source ${MD_ROOT}/libs/gearmulator-md-mm/source/mc68k)
target_link_libraries(mdcore PUBLIC dsp56kEmu 68kEmu)

set(MD_CORE ${MD_ROOT}/desktop/src/core)	# the kit table and its rules, shared with the desktop plugin

add_executable(md-engine src/child/md_engine.cpp ${MD_CORE}/EngineFeeder.cpp ${MD_CORE}/KitModel.cpp)
target_include_directories(md-engine PRIVATE src/common src/child ${MD_CORE})
target_link_libraries(md-engine PRIVATE mdcore pthread dl)
target_link_options(md-engine PRIVATE -static)

add_library(dsp SHARED src/plugin/md_plugin.cpp ${MD_CORE}/KitModel.cpp)
target_include_directories(dsp PRIVATE src src/common ${MD_CORE})
set_target_properties(dsp PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
target_link_options(dsp PRIVATE -Wl,--no-undefined,--exclude-libs,ALL,-Bsymbolic -static-libstdc++ -static-libgcc)
target_link_libraries(dsp PRIVATE pthread dl)

# md-hosttest: loads dsp.so as Schwung does and drives it from a simulated audio callback (run on the Move)
add_executable(md-hosttest tests/md_hosttest.cpp)
target_include_directories(md-hosttest PRIVATE src)
target_link_libraries(md-hosttest PRIVATE pthread dl)
target_link_options(md-hosttest PRIVATE -static-libstdc++ -static-libgcc)

# md-busybench: a busy 16-track kit's cost per Move block, for 1-3 voice threads, master effects off/on (run on the Move)
add_executable(md-busybench tests/md_busybench.cpp ${MD_CORE}/EngineFeeder.cpp ${MD_CORE}/KitModel.cpp)
target_include_directories(md-busybench PRIVATE ${MD_CORE} src/child)
target_link_libraries(md-busybench PRIVATE mdcore pthread dl)
target_link_options(md-busybench PRIVATE -static)

# the flash-image import on the Move: md-import runs mdProbe (the full emulated Machinedrum, built from libs/gearmulator-md-mm by
# scripts/build.sh) and mdsamples, which sit next to it (tools/mdimport)
add_executable(mdsamples ${MD_ROOT}/tools/mdkits/mdsamples.cpp)
target_link_libraries(mdsamples PRIVATE mdcore pthread dl)
target_link_options(mdsamples PRIVATE -static)
add_executable(md-import ${MD_ROOT}/tools/mdimport/md_import.cpp)
target_link_options(md-import PRIVATE -static)
