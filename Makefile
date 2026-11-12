# riscosshd: SSH server for RISC OS 5, cross-built with GCCSDK in a container.
#
#   make            build everything: dependencies, the sshd binary, the
#                   !SSHd application and the PackMan package in dist/
#   make image      build the GCCSDK cross-compiler container image
#   make engine     show the container engine in use (starts Colima if needed)
#   make install    copy !SSHd into a local RPCEmu HostFS and authorise your key
#   make smoke      run a command and an sftp listing against the server
#                   (make test is the same)
#   make check      host-side unit test of the key parser (needs libssh on the host)
#   make clean      remove the build tree;  make distclean  also the dependencies
#
# Variables you may override: ENGINE (docker|podman), IMAGE, RPCEMU_HOSTFS,
# PUBKEY (default ~/.ssh/id_ed25519.pub), SSH_PORT (2222), SSH_HOST, SSH_USER,
# PROFILE (1: log timing of the server loop, see src/prof.h), WERROR (1: our
# warnings are errors, as in CI).

IMAGE      ?= riscosshd-gccsdk
ENGINE     ?= $(shell tools/container-engine.sh 2>/dev/null)
PUBKEY     ?= $(HOME)/.ssh/id_ed25519.pub
SSH_PORT   ?= 2222
SSH_HOST   ?= localhost
SSH_USER   ?= riscos
VERSION    := $(shell sed -n 's/^Version: *//p' riscos/RiscPkg/Control)
PACKAGE    := dist/SSHd_$(VERSION).zip
PROFILE    ?= 0
# The build date in the binary and the times in the zip: the last commit's.
SOURCE_DATE_EPOCH ?= $(shell git log -1 --format=%ct 2>/dev/null || date +%s)
export SOURCE_DATE_EPOCH
# Which commit this is, for the Info window and the log: a build from main
# after a release is told apart from the release itself.
GIT_DESC   := $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
WERROR     ?= 0
SYSROOT    := deps/sysroot
BINARY     := build/!SSHd/sshd,ff8

# Run a command inside the toolchain container with the repository at /work.
# docker-env.sh points the docker CLI at Colima's socket without the Docker
# Desktop credential helper; it is harmless elsewhere.
# On Linux the container runs as the calling user, so build/ and deps/ are
# not left owned by root; Docker Desktop and Colima map ownership already.
ifeq ($(shell uname -s),Linux)
AS_USER    := -u $(shell id -u):$(shell id -g) -e HOME=/tmp
endif
# CMake's configure output: hidden locally, shown in CI (QUIET= on the
# command line) where it is the first thing to read when a build breaks.
QUIET      ?= >/dev/null

RUN = . tools/docker-env.sh; $(ENGINE) run --rm --platform linux/amd64 $(AS_USER) \
      -e SOURCE_DATE_EPOCH -v "$(CURDIR):/work" -w /work $(IMAGE)

SRC := $(wildcard src/*.c src/*.h) CMakeLists.txt cmake/toolchain-riscos.cmake \
       $(wildcard riscos/!SSHd/*) riscos/RiscPkg/Control
DEPS_INPUTS := tools/build-deps.sh deps/mbedtls_riscos_config.h \
       deps/patches/libssh-riscos.patch cmake/toolchain-riscos.cmake \
       $(wildcard deps/riscos-compat/sys/*.h) docker/Dockerfile

.PHONY: all engine image deps build package sprites install smoke test check clean distclean

all: package

engine:
	@e=$$(tools/container-engine.sh start) && echo "container engine: $$e"

# --- toolchain container ---------------------------------------------------
# riscosdotinfo/riscos-gccsdk-4.7 (GCC 4.7.4, UnixLib, elf2aif; linux/amd64
# only) plus cmake and zip, see docker/Dockerfile.  On Apple Silicon it runs
# under emulation (Colima's binfmt or Docker Desktop's Rosetta).
# .image-built records that the image was built; 'make image' rebuilds it
# regardless, e.g. after the engine lost its images.
define build-image
	@test -n "$(ENGINE)" || { tools/container-engine.sh; exit 1; }
	. tools/docker-env.sh; $(ENGINE) build --platform linux/amd64 -t $(IMAGE) docker
	@touch .image-built
endef
image:
	$(build-image)
.image-built: docker/Dockerfile
	$(build-image)

# --- dependencies: mbedTLS and libssh, static, into deps/sysroot -----------
deps: $(SYSROOT)/lib/libssh.a

$(SYSROOT)/lib/libssh.a: $(DEPS_INPUTS) | .image-built
	$(RUN) sh tools/build-deps.sh

# --- the server --------------------------------------------------------------
sprites:
	python3 -I tools/mksprites.py

build: $(BINARY)

# build/.profile-<0|1> records the PROFILE setting, so switching it rebuilds.
PROFILE_STAMP := build/.profile-$(PROFILE)
$(PROFILE_STAMP):
	@mkdir -p build && rm -f build/.profile-* && touch $@

$(BINARY): $(SYSROOT)/lib/libssh.a $(SRC) $(PROFILE_STAMP)
	$(RUN) sh -c 'cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscos.cmake \
	    -DCMAKE_BUILD_TYPE=Release -DSSHD_PROFILE=$(PROFILE) -DSSHD_WERROR=$(WERROR) -DSSHD_GIT=$(GIT_DESC) $(QUIET) && cmake --build build -j$$(nproc) && \
	    cp -R "riscos/!SSHd/." "build/!SSHd/" && touch "$(BINARY)"'
	@# the app files are copied in the same container run: it owns build/ on CI

# --- PackMan package ---------------------------------------------------------
# Layout: Apps/Network/!SSHd, RiscPkg/Control, RiscPkg/Copyright, zipped with
# ,xxx filetype suffixes.  The Components line in Control carries the
# (Movable LookAt Run) flags, so PackMan can add !SSHd to the boot sequence.
package: $(PACKAGE)

# RiscPkg/Copyright is our licence plus those of the statically linked
# libraries; mkzip.sh makes the zip reproducible.
$(PACKAGE): $(BINARY) riscos/RiscPkg/Control LICENSE tools/mkcopyright.sh tools/mkzip.sh \
            $(wildcard riscos/RiscPkg/licences/*)
	$(RUN) sh -c 'rm -rf build/pkg && mkdir -p build/pkg/Apps/Network build/pkg/RiscPkg dist && \
	    cp -R "build/!SSHd" build/pkg/Apps/Network/ && cp riscos/RiscPkg/Control build/pkg/RiscPkg/ && \
	    sh tools/mkcopyright.sh . build/pkg/RiscPkg/Copyright && sh tools/mkzip.sh build/pkg $(PACKAGE)'
	@echo "package: $(PACKAGE)"

# --- local RPCEmu ------------------------------------------------------------
install: $(BINARY)
	@h=$$(tools/find-rpcemu.sh) && sh tools/rpcemu-install.sh "$$h" "$(PUBKEY)"

test: smoke
smoke:
	@echo "+ ssh -p $(SSH_PORT) $(SSH_USER)@$(SSH_HOST) '*Time'"
	@ssh -p $(SSH_PORT) -o ConnectTimeout=15 -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR $(SSH_USER)@$(SSH_HOST) '*Time'
	@echo "+ sftp: pwd, ls"
	@printf 'pwd\nls\n' | sftp -q -P $(SSH_PORT) -o ConnectTimeout=15 -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -b - $(SSH_USER)@$(SSH_HOST) | head -6

# --- host-side unit test -----------------------------------------------------
# Compiles src/auth.c against the host's libssh (brew install libssh, or
# apt install libssh-dev) with a stdio stand-in for the RISC OS file calls.
check: build/host/test_auth
	@test -f build/host/k1.pub || ssh-keygen -q -t ed25519 -N '' -C 'test key one' -f build/host/k1
	@test -f build/host/k2.pub || ssh-keygen -q -t rsa -b 2048 -N '' -C 'test key two' -f build/host/k2
	build/host/test_auth build/host/k1.pub build/host/k2.pub build/host/authkeys

build/host/test_auth: tests/test_auth.c tests/riscos_stub.c src/auth.c src/log.c src/auth.h src/log.h src/riscos.h
	@pkg-config --exists libssh || { echo "libssh not found on the host (pkg-config libssh)"; exit 1; }
	@mkdir -p build/host
	cc -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Isrc $$(pkg-config --cflags libssh) -o $@ \
	    tests/test_auth.c tests/riscos_stub.c src/auth.c src/log.c $$(pkg-config --libs libssh)

clean:
	rm -rf build dist

distclean: clean
	rm -rf deps/sysroot deps/src deps/dl .image-built
