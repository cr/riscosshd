cmake_minimum_required(VERSION 3.12)
project(riscosshd C)

# Cross-build only: use cmake/toolchain-riscos.cmake (see tools/build.sh).
if(NOT CMAKE_C_COMPILER MATCHES "arm-unknown-riscos")
  message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscos.cmake")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")

find_path(LIBSSH_INCLUDE_DIR libssh/libssh.h REQUIRED)
find_library(LIBSSH_LIBRARY ssh REQUIRED)
find_library(MBEDTLS_LIBRARY mbedtls REQUIRED)
find_library(MBEDX509_LIBRARY mbedx509 REQUIRED)
find_library(MBEDCRYPTO_LIBRARY mbedcrypto REQUIRED)

add_executable(sshd
  src/main.c
  src/server.c
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
  src/riscos_compat.c)
target_include_directories(sshd PRIVATE ${LIBSSH_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/deps/riscos-compat)
# Package version from the RiscPkg Control file, shown in the Info window.
file(STRINGS ${CMAKE_SOURCE_DIR}/riscos/RiscPkg/Control CONTROL_VERSION REGEX "^Version:")
string(REGEX REPLACE "^Version: *" "" SSHD_VERSION "${CONTROL_VERSION}")
target_compile_definitions(sshd PRIVATE LIBSSH_STATIC=1 WITH_SERVER=1 SSHD_VERSION="${SSHD_VERSION}")
target_link_libraries(sshd PRIVATE
  ${LIBSSH_LIBRARY} ${MBEDTLS_LIBRARY} ${MBEDX509_LIBRARY} ${MBEDCRYPTO_LIBRARY})
set_target_properties(sshd PROPERTIES OUTPUT_NAME "sshd" SUFFIX ",e1f")

# Convert the static ELF to an AIF absolute (filetype FF8) that runs without
# the ELF loader, and assemble the !SSHd application directory.
set(APP_DIR ${CMAKE_BINARY_DIR}/!SSHd)
add_custom_command(TARGET sshd POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${APP_DIR}
  COMMAND ${RISCOS_ELF2AIF} $<TARGET_FILE:sshd> ${APP_DIR}/sshd,ff8
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/riscos/!SSHd ${APP_DIR}
  COMMENT "Creating !SSHd application")

# Zip in the layout PackMan / riscpkg expect: Apps/Network/!SSHd + RiscPkg/Control.
add_custom_target(package-zip
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/pkg
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pkg/Apps/Network
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${APP_DIR} "${CMAKE_BINARY_DIR}/pkg/Apps/Network/!SSHd"
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/riscos/RiscPkg ${CMAKE_BINARY_DIR}/pkg/RiscPkg
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_BINARY_DIR}/pkg/RiscPkg/Copyright
  COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_BINARY_DIR}/pkg zip -q -r -9 ${CMAKE_BINARY_DIR}/SSHd.zip .
  DEPENDS sshd
  COMMENT "Creating SSHd.zip")
