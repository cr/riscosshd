# CMake toolchain file for the GCCSDK arm-unknown-riscos cross-compiler.
# Mirrors $GCCSDK_INSTALL_ENV/toolchain-riscos.cmake from the autobuilder but
# takes its paths from the environment / cache so it works in the Docker image
# and on a native GCCSDK install alike.
#
#   GCCSDK_INSTALL_CROSSBIN  directory holding arm-unknown-riscos-gcc
#   RISCOS_SYSROOT           where cross-built libraries are installed
set(CMAKE_SYSTEM_NAME GNU)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_SYSTEM_VERSION 1)

if(NOT DEFINED ENV{GCCSDK_INSTALL_CROSSBIN})
  set(ENV{GCCSDK_INSTALL_CROSSBIN} /home/riscos/cross/bin)
endif()
set(GCCSDK_BIN "$ENV{GCCSDK_INSTALL_CROSSBIN}" CACHE PATH "GCCSDK cross bin dir")
set(RISCOS_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../deps/sysroot" CACHE PATH "Cross-built library prefix")

set(CMAKE_C_COMPILER   ${GCCSDK_BIN}/arm-unknown-riscos-gcc)
set(CMAKE_CXX_COMPILER ${GCCSDK_BIN}/arm-unknown-riscos-g++)
set(CMAKE_AR           ${GCCSDK_BIN}/arm-unknown-riscos-ar CACHE FILEPATH "")
set(CMAKE_RANLIB       ${GCCSDK_BIN}/arm-unknown-riscos-ranlib CACHE FILEPATH "")
set(CMAKE_C_COMPILER_TARGET arm-unknown-riscos)
set(RISCOS_ELF2AIF     ${GCCSDK_BIN}/elf2aif CACHE FILEPATH "ELF to AIF converter")

set(CMAKE_FIND_ROOT_PATH ${RISCOS_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} ${RISCOS_SYSROOT}/lib/pkgconfig)

# UnixLib is single-threaded for our purposes: no pthreads.
set(CMAKE_THREAD_LIBS_INIT "" CACHE STRING "" FORCE)
set(CMAKE_HAVE_THREADS_LIBRARY 0 CACHE INTERNAL "")
set(CMAKE_USE_PTHREADS_INIT 0 CACHE INTERNAL "")
set(Threads_FOUND TRUE CACHE INTERNAL "")

# Everything is static: there is no dynamic linker for AIF binaries.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
