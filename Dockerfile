# Build and run fms-m on Debian 13 (trixie).
#
#   docker build -t fms-m .
#   docker run --rm -p 1935:1935 -p 8080:8080 -v "$PWD/rec:/rec" fms-m
#
# Run the unit tests as part of the build:
#   docker build -t fms-m --build-arg RUN_TESTS=1 .
#
# AddressSanitizer build:
#   docker build -t fms-m-asan --build-arg SANITIZE=address .
#
# Two stages: the runtime image carries the binary and the shared libraries it
# needs, not the compiler, the headers or the source tree.

# ---- build ------------------------------------------------------------------
FROM debian:13 AS build

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

ARG SANITIZE=""
ARG RUN_TESTS=0

# interop drives real rtmpdump/ffmpeg/rtmfp-cpp clients, none of which are in
# this image.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DSANITIZE="${SANITIZE}" \
        -DBUILD_TESTS=ON \
    && cmake --build build -j"$(nproc)" \
    && if [ "${RUN_TESTS}" != "0" ]; then ctest --test-dir build --output-on-failure -E interop; fi

# The runtime packages are whatever owns the libraries the binary actually links
# against. Debian 13 ships two boost versions and renamed libssl3 to libssl3t64,
# so naming them by hand here dates the moment a base image moves.
# readlink -f is required: ldd reports /lib/<triplet>/... but dpkg knows those
# files under /usr/lib/<triplet>/..., and dpkg -S does not follow the symlink.
# Resolving nothing would yield an image that builds and then dies at startup on
# a missing .so, so an empty result is a build failure.
RUN ldd build/fms-m \
    | awk '$3 ~ /^\// { print $3 }' \
    | xargs -r readlink -f \
    | xargs -r dpkg -S \
    | cut -d: -f1 \
    | sort -u > /runtime-deps.txt \
    && test -s /runtime-deps.txt \
    && grep -q boost /runtime-deps.txt

# ---- runtime ----------------------------------------------------------------
FROM debian:13-slim

COPY --from=build /runtime-deps.txt /tmp/runtime-deps.txt
RUN apt-get update \
    && xargs -a /tmp/runtime-deps.txt apt-get install -y --no-install-recommends \
    && rm -rf /var/lib/apt/lists/* /tmp/runtime-deps.txt

# Unprivileged: the server binds 1935/8080, neither of which needs root.
RUN groupadd --gid 10001 fms \
    && useradd --no-create-home --uid 10001 --gid 10001 --shell /usr/sbin/nologin fms \
    && mkdir -p /rec /logs \
    && chown fms:fms /rec /logs

COPY --from=build /src/build/fms-m /usr/local/bin/fms-m

USER fms

# RTMP/RTMPE (TCP), RTMPT (HTTP tunnel, TCP), RTMFP (UDP)
EXPOSE 1935/tcp 8080/tcp 1935/udp

ENTRYPOINT ["/usr/local/bin/fms-m"]
CMD ["--bind-address", "0.0.0.0", \
     "--rtmp-port", "1935", \
     "--rtmpt-port", "8080", \
     "--output-folder", "/rec", \
     "--log-path", "/logs", \
     "--threads", "4"]
