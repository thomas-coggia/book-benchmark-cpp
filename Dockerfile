FROM amd64/ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain: clang-19 + libstdc++-14 (clang-19 picks GCC 14's libstdc++ automatically once
# both are installed, which gives us C++23 <print> and the rest of the standard library).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang-19 \
        lld-19 \
        libclang-rt-19-dev \
        gcc-14 \
        g++-14 \
        libstdc++-14-dev \
        cmake \
        python3 \
        pipx \
        ca-certificates \
        git \
    && rm -rf /var/lib/apt/lists/*

# Install conan in an isolated venv under /opt and expose the binary in
# /usr/local/bin so it's on PATH for every user. No PEP 668 override needed.
ENV PIPX_HOME=/opt/pipx PIPX_BIN_DIR=/usr/local/bin
RUN pipx install "conan==2.*"

WORKDIR /app

CMD ["/bin/bash"]
