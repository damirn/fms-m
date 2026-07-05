# Adobe Flash Media Server 4.5 (2011, linux x86-64) as a ground-truth RTMP oracle.
#
# The binaries need only GLIBC<=2.4 + NSPR at runtime; OpenSSL 1.0.0, expat and
# the adbe libs are bundled inside the tarball. fmsedge additionally links the
# legacy libcap.so.1 (v1) -- the 5 process-cap functions it uses
# (cap_get_proc/set_proc/init/set_flag/free) are ABI-identical in v2, so we point
# the old soname at the installed libcap.so.2.
#
# Build/run on Apple Silicon or any host via amd64 emulation:
#   docker build  --platform linux/amd64 -t fms45 .
#   docker run -d --platform linux/amd64 --name fms45 -p 11935:1935 fms45
#
# The FMS 4.5 tarball is Adobe-proprietary and is NOT redistributed here. Drop
# FlashMediaServer4.5_x64.tar.gz into this directory before building (see README).
FROM debian:bullseye-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libnspr4 libcap2 procps psmisc net-tools tcpdump ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf "$(find / -name 'libcap.so.2*' -type f | head -1)" /usr/lib/x86_64-linux-gnu/libcap.so.1

COPY FlashMediaServer4.5_x64.tar.gz /tmp/fms.tar.gz
RUN mkdir -p /opt && tar xzf /tmp/fms.tar.gz -C /opt \
    && mv /opt/FMS_4_5_0_r297 /opt/fms && rm /tmp/fms.tar.gz

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

WORKDIR /opt/fms
EXPOSE 1935 1111
ENTRYPOINT ["/entrypoint.sh"]
