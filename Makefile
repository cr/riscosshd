# riscosshd: SSH server for RISC OS 5, cross-built with GCCSDK in a container.
#
#   make            build everything: dependencies, the sshd binary, the
#                   !SSHd application and the PackMan package in dist/
#   make image      build the GCCSDK cross-compiler container image
#   make engine     show the container engine in use (starts Colima if needed)
#   make install    copy !SSHd into a local RPCEmu HostFS and authorise your key
#   make test       run a command and an sftp listing against the server
#   make clean      remove the build tree;  make distclean  also the dependencies
#
# Variables you may override: ENGINE (docker|podman), IMAGE, RPCEMU_HOSTFS,
# PUBKEY (default ~/.ssh/id_ed25519.pub), SSH_PORT (2222), SSH_HOST, SSH_USER.

IMAGE      ?= riscosshd-gccsdk
ENGINE     ?= $(shell tools/container-engine.sh 2>/dev/null)
PUBKEY     ?= $(HOME)/.ssh/id_ed25519.pub
SSH_PORT   ?= 2222
SSH_HOST   ?= localhost
SSH_USER   ?= riscos
VERSION    := $(shell sed -n 's/^Version: *//p' riscos/RiscPkg/Control)
PACKAGE    := dist/SSHd_$(VERSION).zip
SYSROOT    := deps/sysroot
BINARY     := build/!SSHd/sshd,ff8

# Run a command inside the toolchain container with the repository at /work.
# docker-env.sh points the docker CLI at Colima's socket without the Docker
# Desktop credential helper; it is harmless elsewhere.
RUN = . tools/docker-env.sh; $(ENGINE) run --rm --platform linux/amd64 \
      -v "$(CURDIR):/work" -w /work $(IMAGE)

SRC := $(wildcard src/*.c src/*.h) CMakeLists.txt cmake/toolchain-riscos.cmake \
       $(wildcard riscos/!SSHd/*) riscos/RiscPkg/Control
DEPS_INPUTS := tools/build-deps.sh deps/mbedtls_riscos_config.h \
       deps/patches/libssh-riscos.patch cmake/toolchain-riscos.cmake \
       $(wildcard deps/riscos-compat/sys/*.h)

.PHONY: all engine image deps build package sprites install test clean distclean

all: package

engine:
	@e=$$(tools/container-engine.sh start) && echo "container engine: $$e"

# --- toolchain container ---------------------------------------------------
# riscosdotinfo/riscos-gccsdk-4.7 (GCC 4.7.4, UnixLib, elf2aif; linux/amd64
# only) plus cmake and zip, see docker/Dockerfile.  On Apple Silicon it runs
# under emulation (Colima's binfmt or Docker Desktop's Rosetta).
image: docker/Dockerfile
	@test -n "$(ENGINE)" || { tools/container-engine.sh; exit 1; }
	. tools/docker-env.sh; $(ENGINE) build --platform linux/amd64 -t $(IMAGE) docker
	@touch .image-built

.image-built: docker/Dockerfile
	@$(MAKE) image

# --- dependencies: mbedTLS and libssh, static, into deps/sysroot -----------
deps: $(SYSROOT)/lib/libssh.a

$(SYSROOT)/lib/libssh.a: .image-built $(DEPS_INPUTS)
	$(RUN) sh tools/build-deps.sh

# --- the server --------------------------------------------------------------
sprites:
	python3 -I tools/mksprites.py

build: $(BINARY)

$(BINARY): $(SYSROOT)/lib/libssh.a $(SRC)
	$(RUN) sh -c 'cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscos.cmake \
	    -DRISCOS_SYSROOT=/work/$(SYSROOT) -DCMAKE_BUILD_TYPE=Release >/dev/null && \
	    cmake --build build -j$$(nproc)'

# --- PackMan package ---------------------------------------------------------
# Layout: Apps/Network/!SSHd, RiscPkg/Control, RiscPkg/Copyright, zipped with
# ,xxx filetype suffixes.  The Components line in Control carries the
# (Movable LookAt Run) flags, so PackMan can add !SSHd to the boot sequence.
package: $(PACKAGE)

$(PACKAGE): $(BINARY) riscos/RiscPkg/Control LICENSE
	$(RUN) sh -c 'cmake --build build --target package-zip >/dev/null'
	@mkdir -p dist && cp build/SSHd.zip $(PACKAGE)
	@echo "package: $(PACKAGE)"

# --- local RPCEmu ------------------------------------------------------------
install: $(BINARY)
	@h=$$(tools/find-rpcemu.sh) && sh tools/rpcemu-install.sh "$$h" "$(PUBKEY)"

test:
	@echo "+ ssh -p $(SSH_PORT) $(SSH_USER)@$(SSH_HOST) '*Time'"
	@ssh -p $(SSH_PORT) -o ConnectTimeout=15 -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR $(SSH_USER)@$(SSH_HOST) '*Time'
	@echo "+ sftp: pwd, ls"
	@printf 'pwd\nls\n' | sftp -q -P $(SSH_PORT) -o ConnectTimeout=15 -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -b - $(SSH_USER)@$(SSH_HOST) | head -6

clean:
	rm -rf build dist

distclean: clean
	rm -rf deps/sysroot deps/src deps/dl .image-built
