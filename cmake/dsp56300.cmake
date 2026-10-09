# DSP56300 emulator (GPLv3, https://github.com/dsp56300/dsp56300), forked at sd88me/dsp56300, branch
# recomp-fixes (this repo's libs/dsp56300 submodule: the arm32 static-recompiler fork plus the boot-
# protocol cache-invalidation and Opcodes SIOF fixes found while bringing up this port's own recompiler,
# see HANDOFF.md). Only the emulator's static libraries are built: its own top-level CMake project adds
# tools and global optimisation flags this project does not want.
set(MD_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(MD_DSP56300_DIR "${MD_REPO_ROOT}/libs/dsp56300" CACHE PATH
    "dsp56300 checkout; defaults to this repo's own submodule")
if(NOT EXISTS "${MD_DSP56300_DIR}/source/dsp56kEmu/dsp.h")
  message(FATAL_ERROR "dsp56300 not found at ${MD_DSP56300_DIR} -- run 'git submodule update --init' first, "
                       "or pass -DMD_DSP56300_DIR=<an existing checkout>")
endif()

set(ASMJIT_STATIC TRUE)
set(ASMJIT_NO_INSTALL TRUE)
add_subdirectory("${MD_DSP56300_DIR}/source/asmjit"     "${CMAKE_BINARY_DIR}/dsp56300/asmjit"     EXCLUDE_FROM_ALL)
add_subdirectory("${MD_DSP56300_DIR}/source/dsp56kBase" "${CMAKE_BINARY_DIR}/dsp56300/dsp56kBase" EXCLUDE_FROM_ALL)
add_subdirectory("${MD_DSP56300_DIR}/source/dsp56kEmu"  "${CMAKE_BINARY_DIR}/dsp56300/dsp56kEmu"  EXCLUDE_FROM_ALL)
if(UNIX AND NOT APPLE)   # dsp56kEmu links vtuneSdk unconditionally on Linux (its own CMakeLists.txt)
  add_subdirectory("${MD_DSP56300_DIR}/source/vtuneSdk" "${CMAKE_BINARY_DIR}/dsp56300/vtuneSdk" EXCLUDE_FROM_ALL)
endif()

# The 68k (ColdFire host) emulator, Musashi, vendored inside gearmulator-md-mm (used here only for
# MachineRunner's coefficient functions, not for full-system emulation - see engine/MachineRunner.h).
add_subdirectory("${MD_REPO_ROOT}/libs/gearmulator-md-mm/source/mc68k"
                  "${CMAKE_BINARY_DIR}/mc68k" EXCLUDE_FROM_ALL)
