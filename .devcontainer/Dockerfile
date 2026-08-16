# forgectrl development container: the panel dev server (Python 3,
# standard library only) plus everything the host-side CMake build and
# its unit tests need, mirroring the CI job in .github/workflows/build.yml.
FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake gdb pkg-config \
        libulfius-dev libjpeg-dev zlib1g-dev \
        python3 curl jq \
 && apt-get clean && rm -rf /var/lib/apt/lists/*
