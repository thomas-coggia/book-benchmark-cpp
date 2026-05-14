FROM amd64/ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain: clang-19 + libstdc++-14 (clang-19 picks GCC 14's libstdc++ automatically once
# both are installed, which gives us C++23 <print> and the rest of the standard library).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang-19 \
        lld-19 \
        gcc-14 \
        g++-14 \
        libstdc++-14-dev \
        cmake \
        python3 \
        python3-pip \
        ca-certificates \
        git \
    && rm -rf /var/lib/apt/lists/*

# Conan 2 — single source of truth for third-party deps. Production code has none; the only
# external library declared is GoogleTest (test_requires).
RUN pip install --break-system-packages "conan==2.*"

# Make clang-19 the default system compiler so:
#   * `conan profile detect --force` records clang-19,
#   * any `conan install` building gtest from source picks clang-19 too,
#   * the README's vanilla `conan install ... -s compiler=clang ...` line just works.
ENV CC=/usr/bin/clang-19 \
    CXX=/usr/bin/clang++-19

# First-run helper: write a default Conan profile if none exists yet. Failures are
# tolerated at image-build time (the user can always re-run inside the container).
RUN conan profile detect --force || true

WORKDIR /app

CMD ["/bin/bash"]
