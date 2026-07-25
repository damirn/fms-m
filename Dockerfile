# Build and run fms-m on Debian 13 (trixie).
#
#   docker build -t fms-m .
#   docker run --rm -p 1935:1935 -p 8080:8080 -v "$PWD/rec:/rec" fms-m
#
# AddressSanitizer build:
#   docker build -t fms-m-asan \
#     --build-arg CXXFLAGS_EXTRA="-fsanitize=address -fno-omit-frame-pointer -g -O1" \
#     --build-arg LDFLAGS_EXTRA="-fsanitize=address" .
FROM debian:13

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libboost-all-dev \
        libssl-dev \
        libspeex-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Empty by default (release-ish build); set to enable a sanitizer, see header.
ARG CXXFLAGS_EXTRA=""
ARG LDFLAGS_EXTRA=""

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_CXX_FLAGS="${CXXFLAGS_EXTRA}" \
        -DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS_EXTRA}" \
    && cmake --build build -j"$(nproc)"

RUN mkdir -p /rec /logs

# RTMP/RTMPE (TCP), RTMPT (HTTP tunnel, TCP), RTMFP (UDP)
EXPOSE 1935/tcp 8080/tcp 1935/udp

ENTRYPOINT ["/src/build/fms-m"]
CMD ["--bind-address", "0.0.0.0", \
     "--rtmp-port", "1935", \
     "--rtmpt-port", "8080", \
     "--output-folder", "/rec", \
     "--log-path", "/logs", \
     "--threads", "4"]
