FROM mcr.microsoft.com/devcontainers/cpp:ubuntu-24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang \
    cmake \
    ninja-build \
    gdb \
    pkg-config \
    libraptor2-dev \
    graphviz \
    clang-format \
    clang-tidy \
    python3 \
    ca-certificates \
    curl \
    unzip \
    default-jre-headless \
    && rm -rf /var/lib/apt/lists/*
