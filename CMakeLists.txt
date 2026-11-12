cmake_minimum_required(VERSION 3.13)
project(riscosshd C)

# Cross-build only: use cmake/toolchain-riscos.cmake (the Makefile does).
if(NOT CMAKE_C_COMPILER MATCHES "arm-unknown-riscos")
  message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscos.cmake")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wshadow -Wmissing-prototypes -Wstrict-prototypes -Wformat=2 -Wundef -Wwrite-strings -Wold-style-definition -ffunction-sections -fdata-sections")   # optimisation comes from CMAKE_BUILD_TYPE

# (find_* REQUIRED needs CMake 3.18; the container has 3.16, so check by hand.)
find_path(LIBSSH_INCLUDE_DIR libssh/libssh.h)
find_library(LIBSSH_LIBRARY ssh)
find_library(MBEDCRYPTO_LIBRARY mbedcrypto)
foreach(v LIBSSH_INCLUDE_DIR LIBSSH_LIBRARY MBEDCRYPTO_LIBRARY)
  if(NOT ${v})
    message(FATAL_ERROR "${v} not found in ${RISCOS_SYSROOT}: run 'make deps' first")
  endif()
endforeach()

add_executable(sshd
  src/main.c
  src/server.c
  src/buf.c
  src/prof.c
  src/msgs.c
  src/backend_exec.c
  src/backend_shell.c
  src/auth.c
  src/hostkey.c
  src/riscos.c
  src/riscos_entropy.c
  src/log.c
  src/wimp.c
  src/keyfetch.c
  src/backend_taskwindow.c
  src/backend_sftp.c
  src/riscos_sftp.c)
target_include_directories(sshd SYSTEM PRIVATE ${LIBSSH_INCLUDE_DIR})   # their warnings are not ours
# Package version from the RiscPkg Control file, shown in the Info window.
file(STRINGS ${CMAKE_SOURCE_DIR}/riscos/RiscPkg/Control CONTROL_VERSION REGEX "^Version:")
string(REGEX REPLACE "^Version: *" "" SSHD_VERSION "${CONTROL_VERSION}")
# Build date for the Info window: SOURCE_DATE_EPOCH when set (the Makefile
# sets the last commit's time), so the same commit builds the same binary.
string(TIMESTAMP SSHD_BUILD_DATE "%d %b %Y" UTC)
target_compile_definitions(sshd PRIVATE WITH_SERVER=1 SSHD_VERSION="${SSHD_VERSION}"
                           SSHD_BUILD_DATE="${SSHD_BUILD_DATE}")
# make WERROR=1 (CI): warnings in our own code fail the build
option(SSHD_WERROR "Treat warnings in src/ as errors" OFF)
if(SSHD_WERROR)
  target_compile_options(sshd PRIVATE -Werror)
endif()
# make PROFILE=1: time the server loop and log the totals (src/prof.h)
option(SSHD_PROFILE "Log timing of the server loop" OFF)
if(SSHD_PROFILE)
  target_compile_definitions(sshd PRIVATE SSHD_PROFILE=1)
endif()
target_link_libraries(sshd PRIVATE ${LIBSSH_LIBRARY} ${MBEDCRYPTO_LIBRARY})
# Strip: UnixLib backtraces use embedded function names, not ELF symbols.
target_link_options(sshd PRIVATE -s -Wl,--gc-sections)   # drop the library code nothing references
set_target_properties(sshd PROPERTIES OUTPUT_NAME "sshd" SUFFIX ",e1f")

# Convert the static ELF to an AIF absolute (filetype FF8) that runs without
# the ELF loader.  The Makefile copies riscos/!SSHd's other files next to it
# after every build, so edits to them arrive without a relink.
set(APP_DIR ${CMAKE_BINARY_DIR}/!SSHd)
add_custom_command(TARGET sshd POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${APP_DIR}
  COMMAND ${RISCOS_ELF2AIF} $<TARGET_FILE:sshd> ${APP_DIR}/sshd,ff8
  COMMENT "Creating !SSHd application")

# Zip in the layout PackMan / riscpkg expect: Apps/Network/!SSHd + RiscPkg/Control
# + RiscPkg/Copyright (our licence plus those of the statically linked libraries).
add_custom_target(package-zip
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/pkg
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pkg/Apps/Network
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${APP_DIR} "${CMAKE_BINARY_DIR}/pkg/Apps/Network/!SSHd"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pkg/RiscPkg
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/riscos/RiscPkg/Control ${CMAKE_BINARY_DIR}/pkg/RiscPkg/Control
  COMMAND sh ${CMAKE_SOURCE_DIR}/tools/mkcopyright.sh ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/pkg/RiscPkg/Copyright
  COMMAND sh ${CMAKE_SOURCE_DIR}/tools/mkzip.sh ${CMAKE_BINARY_DIR}/pkg ${CMAKE_BINARY_DIR}/SSHd.zip
  DEPENDS sshd
  COMMENT "Creating SSHd.zip")
