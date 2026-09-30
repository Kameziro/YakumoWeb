# Build environment for the web port.
#
# One image holds both toolchains the web build needs:
# - the native Linux toolchain (Debian 13, as in docs/BUILDING.md), which builds
#   the bootstrap Yakumo that runs `--install` and the recompiler behind
#   generate.sh;
# - the Emscripten SDK, which compiles the port to WebAssembly.
FROM debian:trixie

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build python3 python3-pip ccache pkg-config \
        libsdl3-dev libvulkan-dev glslang-tools \
        git ca-certificates curl xz-utils bzip2 nodejs \
    && rm -rf /var/lib/apt/lists/*

# Emscripten, pinned so rebuilds stay reproducible.
ARG EMSDK_VERSION=6.0.10
ENV EMSDK=/opt/emsdk
RUN git clone --depth 1 https://github.com/emscripten-core/emsdk.git $EMSDK \
    && $EMSDK/emsdk install $EMSDK_VERSION \
    && $EMSDK/emsdk activate $EMSDK_VERSION \
    && echo ". $EMSDK/emsdk_env.sh > /dev/null 2>&1" >> /etc/bash.bashrc

# ccache lives on a volume shared by every build (see run.sh).
ENV CCACHE_DIR=/ccache
# The recompiled code nests deeply: the native bootstrap needs a 64 MiB stack.
RUN echo "ulimit -s 65536" >> /etc/bash.bashrc

WORKDIR /src
CMD ["bash"]
