# ── SS_PSI build environment ──────────────────────────────────────────────────
# Builds and installs:
#   emp-tool  (header-only + shared lib, AES-NI)
#   SS_PSI    (this project)
#
# Build image:
#   docker build -t ss_psi .
#
# Run a test (3 terminals, same container network):
#   docker run --rm -it ss_psi bash
#   # inside: run test binaries as shown in each test file's header comment

FROM ubuntu:22.04

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# ── System dependencies ───────────────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        git \
        libssl-dev \
        libgmp-dev \
        wget \
        ca-certificates \
        gnupg \
        socat \
    && rm -rf /var/lib/apt/lists/*

# ── CMake ≥ 3.25 (Ubuntu 22.04 ships 3.22 which is too old for emp-tool) ─────
RUN wget -qO /tmp/cmake.sh \
        https://github.com/Kitware/CMake/releases/download/v3.29.6/cmake-3.29.6-linux-x86_64.sh \
    && sh /tmp/cmake.sh --prefix=/usr/local --skip-license \
    && rm /tmp/cmake.sh

# ── emp-tool ──────────────────────────────────────────────────────────────────
# Requires AES-NI (-maes) and SSE4.2 (-msse4.2); the CMakeLists in emp-tool
# sets -march=native automatically when built with cmake.
WORKDIR /opt
RUN git clone --depth 1 https://github.com/emp-toolkit/emp-tool.git && \
    cmake -S emp-tool -B emp-tool/build \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local && \
    cmake --build  emp-tool/build --parallel $(nproc) && \
    cmake --install emp-tool/build && \
    rm -rf emp-tool

# ── SS_PSI ────────────────────────────────────────────────────────────────────
WORKDIR /workspace
COPY . .

RUN cmake -S . -B build \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_PREFIX_PATH=/usr/local \
    && cmake --build build --parallel $(nproc)

# Default: drop into a shell so tests can be run manually
CMD ["/bin/bash"]
