# Toolchain image for Chronicle and ChronicleTwo. Built locally, one tag per
# checkout state: docker build --build-arg KZ_IMAGE_ID=<tag> -t kazarov-dev:<tag> -f image/Dockerfile .
#
# linux/amd64 only: wibo runs the Windows-hosted Metrowerks compiler and linker
# and needs a real x86_64 Linux kernel.
#
# Debian for glibc (the toolchain binaries are glibc-linked), trixie because
# binutils-mips-ps2-decompals needs glibc 2.38.

# SatansFiddle, the compiler wrapper that stands in for statefix in ChronicleTwo.
FROM debian:trixie-slim@sha256:f6b08b09a05725f9431502d3b5714f5c4576e82c1a84fa1debe0c24a7e0c3ec4 AS satansfiddle-build
ARG SATANSFIDDLE_REV=365415fa2fd7bf69e899044b704b571a64ed4e6c
RUN printf 'Acquire::Retries "5";\nAcquire::http::Timeout "30";\nAcquire::https::Timeout "30";\n' > /etc/apt/apt.conf.d/80-retries
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates git cargo build-essential liblldb-19-dev \
    && rm -rf /var/lib/apt/lists/*
ENV LLDB_INCLUDE=/usr/lib/llvm-19/include
ENV LLDB_LIB_DIR=/usr/lib/llvm-19/lib
WORKDIR /satansfiddle
RUN git init \
    && git remote add origin https://github.com/Adubbz/SatansFiddle.git \
    && git fetch --depth 1 origin ${SATANSFIDDLE_REV} \
    && git checkout --detach FETCH_HEAD
COPY image/patches/satansfiddle-nested-arguments.patch /tmp/satansfiddle-nested-arguments.patch
RUN git apply --check /tmp/satansfiddle-nested-arguments.patch \
    && git apply /tmp/satansfiddle-nested-arguments.patch
COPY image/patches/satansfiddle-control-context.patch /tmp/satansfiddle-control-context.patch
RUN git apply --check /tmp/satansfiddle-control-context.patch \
    && git apply /tmp/satansfiddle-control-context.patch
COPY image/patches/satansfiddle-placement-new.patch /tmp/satansfiddle-placement-new.patch
RUN git apply --check /tmp/satansfiddle-placement-new.patch \
    && git apply /tmp/satansfiddle-placement-new.patch
COPY image/patches/satansfiddle-external-constructors.patch /tmp/satansfiddle-external-constructors.patch
RUN git apply --check /tmp/satansfiddle-external-constructors.patch \
    && git apply /tmp/satansfiddle-external-constructors.patch
# Rust 1.85 can place native libraries before the LLDB C++ archive, so the
# stage-local flags supply the trailing libraries to the CLI, the test runners
# and the dependency build scripts.
ENV RUSTFLAGS="-L native=/usr/lib/llvm-19/lib -C link-arg=-llldb -C link-arg=-lstdc++"
RUN cargo build --release --locked --jobs 3 \
    && cp target/release/satansfiddle /satansfiddle-production \
    && cargo test --release --locked --jobs 3 --features hook-test-faults \
    && mkdir /satansfiddle-tests \
    && cp target/release/satansfiddle /satansfiddle-tests/satansfiddle-fault-test \
    && find target/release/deps -maxdepth 1 -name 'compiler_cli-*' -type f -executable \
        -exec cp {} /satansfiddle-tests/ \;

#
# Image
#
FROM debian:trixie-slim@sha256:f6b08b09a05725f9431502d3b5714f5c4576e82c1a84fa1debe0c24a7e0c3ec4

# Tool versions
ARG BINUTILS_VERSION=v0.10
ARG BINUTILS_SHA256=9fe31ea3ee1a37536f9f0e2e12c668e7c3cb99e59f5154bd2f1fc4762473094c
ARG OBJDIFF_VERSION=v3.7.3
ARG OBJDIFF_SHA256=1c8a7564970e86b548f0921908d36e5b54646fe72f86e5c82b8c0eba64020cea
ARG LLVM_VERSION=23

ENV DEBIAN_FRONTEND=noninteractive
RUN printf 'Acquire::Retries "5";\nAcquire::http::Timeout "30";\nAcquire::https::Timeout "30";\n' > /etc/apt/apt.conf.d/80-retries
ENV BINUTILS=/usr/local/binutils-mips-ps2-decompals
ENV VIRTUAL_ENV=/opt/venv
ENV PATH=${VIRTUAL_ENV}/bin:/usr/lib/llvm-${LLVM_VERSION}/bin:${PATH}:${BINUTILS}
ENV M2C=/opt/kazarov/m2c/m2c.py
ENV MWCCPS2_DEBUGGER_DIR=/opt/kazarov/mwccps2-debugger
ENV LLDB_DEBUGSERVER_PATH=/usr/lib/llvm-19/bin/lldb-server

# Build tools, debuggers, and the libraries the PC port links against.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        coreutils \
        git \
        gnupg \
        sudo \
        unzip \
        util-linux \
        wget \
        less \
        python3 \
        python3-venv \
        python3-pytest \
        cmake \
        ninja-build \
        gdb \
        lldb-19 \
        build-essential \
        doxygen \
        libsdl3-dev \
        pkg-config \
        libxkbcommon-dev \
        libwayland-dev \
        libx11-dev \
        libxcursor-dev \
        libxi-dev \
        libxrandr-dev \
        libegl1-mesa-dev \
        libvulkan-dev \
        mesa-vulkan-drivers \
        vulkan-validationlayers \
        nlohmann-json3-dev \
        glslang-tools \
        libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

# clang, lld and llvm for the PC port, with compiler-rt for its AddressSanitizer build. Debian stops at
# LLVM 19, so these come from apt.llvm.org and install under /usr/lib/llvm-<version>/bin.
RUN wget --tries=5 --timeout=30 --retry-connrefused -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
    && echo "deb https://apt.llvm.org/trixie/ llvm-toolchain-trixie-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        clang-${LLVM_VERSION} \
        clangd-${LLVM_VERSION} \
        clang-format-${LLVM_VERSION} \
        clang-tidy-${LLVM_VERSION} \
        lld-${LLVM_VERSION} \
        llvm-${LLVM_VERSION} \
        libclang-rt-${LLVM_VERSION}-dev \
    && rm -rf /var/lib/apt/lists/*

# Assembler, objcopy, objdump, readelf and nm for the PS2 decompilation projects.
RUN wget --tries=5 --timeout=30 --retry-connrefused -O /tmp/binutils.tar.gz \
        https://github.com/decompals/binutils-mips-ps2-decompals/releases/download/${BINUTILS_VERSION}/binutils-mips-ps2-decompals-linux-x86-64.tar.gz \
    && echo "${BINUTILS_SHA256}  /tmp/binutils.tar.gz" | sha256sum -c - \
    && mkdir -p ${BINUTILS} \
    && tar xzf /tmp/binutils.tar.gz -C ${BINUTILS} \
    && rm /tmp/binutils.tar.gz

# wibo runs the Windows-hosted Metrowerks compiler and linker. The pinned image
# carries the unstripped loader symbols that the LLDB hooks need.
COPY --from=ghcr.io/decompals/wibo@sha256:3a89948cf841cd6ae2555eb9d8b8ba60c35700852afc5fd9bd62660ef3d201f5 /usr/local/bin/wibo /usr/bin/
COPY --from=satansfiddle-build /satansfiddle-production /usr/local/bin/satansfiddle
COPY --from=satansfiddle-build /satansfiddle-tests/ /usr/local/libexec/satansfiddle-tests/

# The language server and formatting tools share the compiler's signed LLVM packages.
RUN ln -s /usr/lib/llvm-${LLVM_VERSION}/bin/clangd /usr/local/bin/clangd \
    && ln -s /usr/lib/llvm-${LLVM_VERSION}/bin/clangd /usr/bin/clangd

# Single static binary that compares each unit against retail.
RUN wget --tries=5 --timeout=30 --retry-connrefused -O /usr/local/bin/objdiff-cli \
        https://github.com/encounter/objdiff/releases/download/${OBJDIFF_VERSION}/objdiff-cli-linux-x86_64 \
    && echo "${OBJDIFF_SHA256}  /usr/local/bin/objdiff-cli" | sha256sum -c - \
    && chmod +x /usr/local/bin/objdiff-cli

# splat is the disassembler; its MIPS support is an extra and must be named.
# libclang turns the headers into the context m2c reads. graphviz is what m2c
# imports; m2c vendors its own pycparser.
RUN python3 -m venv ${VIRTUAL_ENV} \
    && ${VIRTUAL_ENV}/bin/python -m pip install --no-cache-dir \
        pycdlib==1.22.0 "splat64[mips]==0.50.0" libclang==18.1.1 graphviz==0.20.3 toml==0.10.2 levenshtein==0.27.5 \
        colorama==0.4.6 crunch64==0.6.2 intervaltree==3.1.0 n64img==0.3.3 pygfxd==1.0.5 \
        pylibyaml==0.1.0 pypng==0.20220715.0 pyyaml==6.0.3 rabbitizer==1.16.2 rapidfuzz==3.14.6 \
        sortedcontainers==2.4.0 spimdisasm==1.42.4 tqdm==4.67.1

# m2c and mwccps2-debugger, pinned by commit.
RUN mkdir -p /opt/kazarov/m2c /opt/kazarov/mwccps2-debugger \
    && git -C /opt/kazarov/m2c init \
    && git -C /opt/kazarov/m2c remote add origin https://github.com/matt-kempster/m2c \
    && git -C /opt/kazarov/m2c fetch --depth 1 origin 708d2d2cb2698f091a92492b328f73b24209f72d \
    && git -C /opt/kazarov/m2c checkout --detach FETCH_HEAD \
    && git -C /opt/kazarov/mwccps2-debugger init \
    && git -C /opt/kazarov/mwccps2-debugger remote add origin https://github.com/Raikaru/mwccps2-debugger \
    && git -C /opt/kazarov/mwccps2-debugger fetch --depth 1 origin 3bca7ed2146f85d4e3be478b3d166fce77724499 \
    && git -C /opt/kazarov/mwccps2-debugger checkout --detach FETCH_HEAD

RUN chmod -R a+rX /opt/kazarov

# Minimal LGPL FFmpeg (MPEG-2 video decoder only) for the PC port's movies, linked statically.
COPY tools/ffmpeg/kz-ffmpeg /opt/kazarov/tools/ffmpeg/kz-ffmpeg
RUN apt-get update \
    && apt-get install -y --no-install-recommends nasm \
    && rm -rf /var/lib/apt/lists/* \
    && python3 /opt/kazarov/tools/ffmpeg/kz-ffmpeg --out /opt/kazarov/ffmpeg --jobs "$(nproc)" \
    && chmod -R a+rX /opt/kazarov/ffmpeg
ENV KZ_FFMPEG_DIR=/opt/kazarov/ffmpeg

# The native data publisher is Rust 1.99.0, linked to its C++ CLI through cxx.
# Install the official release only after its digest matches the pinned source.
RUN wget --tries=5 --timeout=30 --retry-connrefused -O /tmp/rust.tar.xz \
        https://static.rust-lang.org/dist/2026-10-01/rust-1.99.0-x86_64-unknown-linux-gnu.tar.xz \
    && echo '891c6366d7100feda0bca4c03ce63f3c9ac827cbebbc283e7433061d42c6a376  /tmp/rust.tar.xz' | sha256sum -c - \
    && tar -xJf /tmp/rust.tar.xz -C /tmp \
    && sh /tmp/rust-1.99.0-x86_64-unknown-linux-gnu/install.sh --prefix=/opt/rust-1.99.0 --disable-ldconfig \
    && rm -rf /tmp/rust.tar.xz /tmp/rust-1.99.0-x86_64-unknown-linux-gnu
ENV PATH=/opt/rust-1.99.0/bin:${PATH}
RUN cargo install cxxbridge-cmd --version 1.0.205 --locked

RUN wget --tries=5 --timeout=30 --retry-connrefused -O /tmp/corrosion.zip \
        https://github.com/corrosion-rs/corrosion/archive/refs/tags/v0.6.1.zip \
    && echo '5c1c5c6ab7196fe4466321c14e9698ec2d0a977c89fd9f1639a37fcffdf604f2  /tmp/corrosion.zip' | sha256sum -c - \
    && unzip -q /tmp/corrosion.zip -d /opt/kazarov \
    && mv /opt/kazarov/corrosion-0.6.1 /opt/kazarov/corrosion \
    && rm /tmp/corrosion.zip
ENV KZ_CORROSION_SOURCE_DIR=/opt/kazarov/corrosion

# Last layer, so a new KZ_IMAGE_ID rebuilds nothing else.
ARG KZ_IMAGE_ID
RUN printf '%s\n' "$KZ_IMAGE_ID" > /opt/kazarov/image-id

