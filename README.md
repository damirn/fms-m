# F Media Server (fms-m)

A lightweight, multi-threaded **RTMP / RTMFP media server** for live audio/video
streaming. It accepts published live streams (as from OBS, ffmpeg, or Flash-era
encoders), relays them to any number of subscribers in real time, and can record
incoming streams to FLV files on disk.

The server speaks the full family of Adobe streaming protocols — plain RTMP,
encrypted RTMPE, TLS-secured RTMPS, HTTP-tunnelled RTMPT (and RTMPTS over TLS),
and UDP-based RTMFP — over an `io_context`-per-core Boost.Asio engine, so a
single process scales across all available CPU cores.

- **Language / build:** C++23, CMake, Boost, OpenSSL 3, Speex
- **Platforms:** macOS (Apple clang) and Linux (GCC/clang) — POSIX only
- **Version:** 2.0.0

---

## Features

- **Live relay** — one publisher fans out to many subscribers with per-client
  queueing and QoS notifications, and a bounded send queue that sheds video
  rather than growing without limit behind a slow client (see
  [Slow consumers](#slow-consumers)).
- **Origin pull** — play `stream@rtmp://origin/app` and, with no local
  publisher, the server bridges the stream in from the remote origin using the
  bundled `fms_helper` relay (see [Stream relay](#stream-relay-origin-pull)).
- **Recording** — publish a stream as type `record` and the server writes it to
  `<output-folder>/<stream>.flv`.
- **Multiple transports** for the same content:
  - **RTMP** — plain TCP (default port `1935`)
  - **RTMPE** — encrypted RTMP (RC4 + Diffie-Hellman handshake), same port
  - **RTMPS** — RTMP over TLS (opt-in; needs a cert/key — see [TLS](#tls-rtmps--rtmpts))
  - **RTMPT** — RTMP tunnelled over HTTP (default port `80`)
  - **RTMPTS** — RTMPT tunnelled over HTTPS (TLS); opt-in, shares the TLS cert/key
  - **RTMFP** — Adobe's UDP real-time protocol (default port `1935/udp`)
- **Codec pass-through** for H.264 video and AAC audio, plus built-in Speex and
  G.711 audio support used by the call application.
- **Admin application** — a password-protected control app exposing live
  application/client/stream statistics and the ability to disconnect clients.
- **Multi-threaded** — a pool of I/O threads, one `io_context` per thread, with
  each connection pinned to a single thread.

---

## Applications

RTMP organises streams under a named *application*. A client connects to
`rtmp://host/<app>/<stream>`. Three applications are registered:

| App name     | Purpose                                                                 |
|--------------|-------------------------------------------------------------------------|
| `media`      | The main A/V app: publish + live fan-out, VOD playback, and recording.  |
| `video_call` | Multi-party video-call variant of `media` (adds call signalling / audio mixing). |
| `admin`      | Administrative control and monitoring (requires a password file).       |

The publish/play/record URL for the media app is
`rtmp://host:1935/media/<streamName>`.

---

## Building

### Dependencies

- A C++23 compiler (Apple clang 16+, or GCC 12+)
- CMake ≥ 3.16
- **Boost** — `date_time`, `log`, `log_setup`, `program_options`, `thread`
  (macOS builds pin Boost **1.76**; Linux uses the system Boost). Boost.System is
  header-only since 1.69 and is deliberately *not* a linked component — newer
  distributions no longer ship the stub library.
- **OpenSSL 3** — `libcrypto` (the **legacy provider** must be available for
  RTMPE, which uses RC4) and `libssl` (for the TLS transports, RTMPS / RTMPTS)
- **Speex** — audio codec
- `pkg-config`

### macOS (Homebrew)

```sh
brew install cmake boost@1.76 openssl@3 speex pkg-config

cmake -S . -B build
cmake --build build -j
```

The CMake configuration locates the keg-only Homebrew `boost@1.76`, `openssl@3`,
and `speex` automatically.

> **Runtime note (macOS):** Boost.Log links against ICU. If the server fails to
> start with a missing `libicudata` error, point the loader at the matching ICU
> keg, e.g.:
>
> ```sh
> DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/opt/icu4c@74/lib ./build/fms-m ...
> ```

### Linux (Debian/Ubuntu)

```sh
sudo apt-get install cmake g++ libboost-all-dev libssl-dev libspeex-dev pkg-config

cmake -S . -B build
cmake --build build -j"$(nproc)"
```

The resulting binary is `build/fms-m`.

---

## Verified build environments

The server has been built **warning-free** and its full RTMP / RTMPE / RTMPS /
RTMPT / RTMPTS / RTMFP functional suite run successfully on the following
combinations, spanning
Boost 1.76 → 1.91, OpenSSL 3.0 → 3.6, GCC 12 → 16 and Clang 16 → 22:

| OS                    | Compiler(s)             | Boost | OpenSSL | Build file          |
|-----------------------|-------------------------|-------|---------|---------------------|
| macOS 15              | Apple Clang 16          | 1.76  | 3.6     | *(Homebrew)*        |
| Debian 12 (bookworm)  | GCC 12.2                | 1.81  | 3.0     | *(host / apt)*      |
| Debian 13 (trixie)    | GCC 14.2                | 1.83  | 3.5     | `Dockerfile`        |
| Ubuntu 26.04          | GCC 15.2 · Clang 21     | 1.90  | 3.5     | `Dockerfile.ubuntu` |
| Arch Linux (rolling)  | GCC 16.1 · Clang 22     | 1.91  | 3.6     | `Dockerfile.arch`   |

Where two compilers are listed, both produce a clean build. The Ubuntu 26.04 and
Arch Linux images are additionally validated clean under **AddressSanitizer**
(including 24 concurrent RTMFP clients). The `Dockerfile*` build files accept
`--build-arg CXX=clang++` and sanitizer flags via `--build-arg CXXFLAGS_EXTRA` /
`LDFLAGS_EXTRA`.

The bundled client tools (`rtmp_client`, `fms_helper`) build alongside the
server on every combination above. The `fms_helper` origin-pull relay is
additionally verified end-to-end — remote publish → relay → local play — on
macOS and Linux.

---

## Running

Quick start — listen for RTMP on 1935, write recordings and logs to the current
directory:

```sh
./build/fms-m
```

A more typical invocation:

```sh
./build/fms-m \
  --bind-address 0.0.0.0 \
  --rtmp-port 1935 \
  --rtmpt-port 8080 \
  --threads 4 \
  --output-folder /var/lib/fms/recordings \
  --log-path /var/log/fms
```

> **Ports below 1024** (such as the default RTMPT port `80`) require elevated
> privileges on Linux. Use `--rtmpt-port 8080` (or similar) when running as a
> normal user.

### TLS (RTMPS / RTMPTS)

The TLS transports are **opt-in**: they are armed only when a certificate *and*
private key are supplied and the corresponding port is set. RTMPS wraps the RTMP
state machine in a TLS stream; RTMPTS runs the RTMPT HTTP tunnel over TLS. Both
share the one cert/key.

A half-configured TLS setup is a **startup error**, not a silent downgrade — the
server refuses to run rather than leave you with a closed port and no
explanation. That covers a TLS port without both `--tls-cert` and `--tls-key`, a
cert without its key, a cert/key pair with neither TLS port set, and a cert that
is present but fails to load.

```sh
# a throwaway self-signed cert for local testing
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
  -days 365 -nodes -subj "/CN=localhost"

./build/fms-m \
  --rtmps-port 443 --rtmpts-port 8443 \
  --tls-cert cert.pem --tls-key key.pem
```

Then `rtmpdump -r rtmps://host/app/stream` (or `rtmpts://…`) plays over TLS; use
a CA-signed cert in production (clients that verify will reject self-signed).

---

## Command-line options

| Short | Long                              | Default   | Description                                            |
|-------|-----------------------------------|-----------|--------------------------------------------------------|
| `-h`  | `--help`                          |           | Print help and exit.                                   |
| `-v`  | `--version`                       |           | Print version information and exit.                    |
| `-b`  | `--bind-address <addr>`           | `0.0.0.0` | Address to bind the listeners to.                      |
| `-R`  | `--rtmp-port <port>`              | `1935`    | RTMP (and RTMPE) TCP listen port.                      |
| `-T`  | `--rtmpt-port <port>`             | `80`      | RTMPT (HTTP-tunnelled RTMP) listen port.               |
| `-K`  | `--rtmfp-port <port>`             | `1935`    | RTMFP (UDP) listen port.                               |
|       | `--rtmps-port <port>`             | *(off)*   | RTMPS (RTMP over TLS) listen port; empty = disabled.   |
|       | `--rtmpts-port <port>`            | *(off)*   | RTMPTS (RTMPT over TLS) listen port; empty = disabled. |
|       | `--tls-cert <path>`               | *(none)*  | PEM certificate chain; required for RTMPS/RTMPTS.      |
|       | `--tls-key <path>`                | *(none)*  | PEM private key; required for RTMPS/RTMPTS.            |
| `-t`  | `--threads <n>`                   | `1`       | Number of I/O threads (one `io_context` per thread).   |
| `-o`  | `--output-folder <path>`          | `.`       | Destination folder for recorded `.flv` files.          |
| `-c`  | `--config-file <path>`            |           | Optional config file (same keys as CLI options).       |
| `-P`  | `--log-path <path>`               | `.`       | Directory for the rotating server log files.           |
| `-f`  | `--log-file <name>`               | *(auto)*  | Explicit log file name.                                |
| `-l`  | `--log-level <n>`                 | `3`       | Log verbosity level.                                   |
| `-a`  | `--auth-plugin <path>`            | *(none)*  | Path to an authentication plugin shared library.       |
| `-F`  | `--password-file <path>`          | `./passwd`| Password file used by the `admin` application.         |
| `-H`  | `--helper-app <path>`             | *(none)*  | Relay helper spawned for origin-pull play (`fms_helper`). |
| `-q`  | `--quality <0..10>`               | `6`       | Speex encoder quality.                                 |
| `-e`  | `--max-queue-bytes <n>`           | `10485760`| Queued outbound bytes per connection before a slow consumer is shed, then dropped (`0` = unbounded). See [Slow consumers](#slow-consumers). |

Options may also be supplied through a config file (`--config-file`) using the
long option names as keys.

---

## Usage examples

The examples use [`ffmpeg`](https://ffmpeg.org/) to publish/play plain RTMP and
[`rtmpdump`](https://rtmpdump.mplayerhq.hu/) for encrypted RTMPE.

### Publish a live stream (RTMP)

```sh
ffmpeg -re -i input.mp4 -c copy -f flv rtmp://127.0.0.1:1935/media/mystream
```

### Play a live stream (RTMP)

```sh
ffmpeg -i rtmp://127.0.0.1:1935/media/mystream -c copy out.flv
# or watch it live:
ffplay rtmp://127.0.0.1:1935/media/mystream
```

### Play an encrypted stream (RTMPE)

```sh
rtmpdump -r "rtmpe://127.0.0.1:1935/media/mystream" --live -o out.flv
```

RTMPE reuses the RTMP port — the client selects encryption via the `rtmpe://`
scheme. The server negotiates the RC4/Diffie-Hellman handshake automatically.

### Play over RTMPT (HTTP tunnel)

```sh
ffmpeg -i rtmpt://127.0.0.1:8080/media/mystream -c copy out.flv
```

### Play / publish over RTMFP (UDP)

RTMFP is Adobe's UDP real-time media protocol. Its original client was Flash
Player, but the server also interoperates with the open-source
[`rtmfp-cpp`](https://github.com/zenomt/rtmfp-cpp) client suite (the RFC 7016
author's implementation). Using its test tools, built from that repository:

```sh
# play a live stream over RTMFP
tcconn -4 'rtmfp://127.0.0.1:1935/media#mystream'

# publish an FLV over RTMFP
tcpublish -4 'rtmfp://127.0.0.1:1935/media#mystream' input.flv
```

`-H -S` relax the per-packet session HMAC and sequence-number requirement; the
server implements both, so they are not needed and the interop matrix runs
without them.

The stream name is passed as the URL fragment (`#mystream`), and RTMFP listens on
UDP `--rtmfp-port` (default `1935/udp`). The server implements the 1024-bit MODP
Diffie-Hellman group (RFC 2409 group 2); modern clients negotiate down to it
automatically. Both directions work — publish over RTMFP and play over RTMP (or
RTMPE/RTMPT), and vice versa — since all transports share the same applications.

---

## Slow consumers

A subscriber that stops reading (or reads slower than the stream) applies TCP
backpressure, and the frames the publisher keeps producing pile up in that
connection's send queue. `--max-queue-bytes` (default 10 MB) bounds that queue
per connection, in three tiers:

| Queued | Behaviour |
|--------|-----------|
| ≤ half the limit | normal — nothing happens |
| over half | shed droppable video down to a quarter, and keep streaming |
| still over the limit afterwards | nothing left to give — disconnect the client |

Shedding drops inter frames first, then whole GOPs, oldest first: a client that
is behind wants the freshest content. **Audio, codec sequence headers and all
control messages are never shed** — audio is a small fraction of the bytes but
far more noticeable when it gaps, and losing a sequence header makes the stream
undecodable for the rest of the session. Shed and disconnect decisions are
logged (rate-limited per connection) and counted into the admin app's dropped
message stats.

Setting `--max-queue-bytes 0` disables the bound entirely, which lets one stuck
client grow the server's memory without limit. It exists for testing and A/B
measurement, not for production.

For reference, Adobe FMS 4.5 bounds the same backlog by bytes (measured at
~8–10 MB, independent of bitrate) and then simply disconnects, without sending
anything at RTMP level and without thinning the stream first. This server keeps
that shape but sheds before it drops, which is gentler and invisible on the
wire. The measurements behind those numbers are in
`docs/slow-consumer.md`, which is a working-tree note and not part of this
repository.

---

## Recording

Recording is triggered by the RTMP **publish type**: a client that calls
`NetStream.publish(name, "record")` causes the server to write the incoming
stream to `<output-folder>/<name>.flv`.

Note that many encoders (including ffmpeg's `-f flv` output) publish with type
`"live"`, which relays but does not record. To record, the publishing client
must request the `record` type. The recorded file is a standard FLV containing
the published audio/video and can be played back with any FLV-capable player.

---

## Stream relay (origin pull)

A client can request a stream that lives on **another** server by playing the
`<stream>@rtmp://<origin-host>/<app>` syntax. When the local server has no
publisher (and no recorded VOD) for `<stream>`, it bridges the stream in from
the origin by spawning the program given by `--helper-app`:

```
fms_helper -r rtmp://<origin-host>/<app> -l rtmp://localhost:<rtmp-port>/<app> -s <stream>
```

The helper plays `<stream>` from the origin and republishes it to the local
server under the same name, feeding the waiting subscriber. The bundled
`fms_helper` tool (built alongside the client, from `client/helper_main.cpp`)
implements exactly this contract — point `--helper-app` at its full path:

```sh
./build/fms-m --helper-app "$PWD/build/fms_helper" ...
```

The relay carries H.264 / AAC (and `onMetaData`) straight through, keeps no
state, and exits when the origin stream ends; the server re-spawns it on the
next remote-stream play.

---

## Admin application & authentication

- **Admin app** — connect to the `admin` application to query live statistics
  (applications, clients, streams, queue stats) and to disconnect clients.
  Access is authenticated against the file given by `--password-file` (default
  `./passwd`). Each line is `user:sha256hash`, optionally salted as
  `user:salt$sha256(salt+password)`.
- **Auth plugin (interface only)** — a loader and plugin ABI exist for an
  external shared-library authenticator (`--auth-plugin <lib>`), intended to
  approve/reject connections and publishes against your own user directory or
  token system. The loader works in isolation, but `authentication_manager` is
  **not instantiated anywhere**, so no plugin is loaded or consulted at runtime.
  The plugin interface is also synchronous, so a plugin that returns
  `indeterminate` has no way to complete.

---

## Testing

```sh
cmake -S . -B build -DBUILD_TESTS=ON      # unit tests
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`BUILD_CLIENT` (on by default) adds the end-to-end `b2b_test` and the
chunk-parser tests.

Two matrices drive real reference clients, so both need those binaries on
`PATH`:

| Script | Runs under `ctest`? | What it covers |
|--------|---------------------|----------------|
| `test/interop/interop.sh` | yes, as test `interop` | Protocol shape: which RTMP user-control events the server sends and the client consumes. Drives rtmpdump, ffmpeg, and rtmfp-cpp's `tcpublish`/`tcconn` (set `RTMFP_CPP` to their build directory). |
| `test/interop/realworld.sh` | no — run it by hand | Outcome: real encodes surviving the round trip (H.264 profiles, AAC/MP3, audio-only, video-only, HEVC/AV1), concurrency and mid-stream failure, each transport carrying media, a 45s durability pull, and the slow-consumer shed. Takes ~4 minutes, which is why it is not registered. |

`RTMPE` skips on ARM (rtmpdump bus-errors on the encrypted handshake); HEVC and
AV1 skip because Enhanced RTMP is not implemented yet — both are deliberate, and
the scripts say so rather than passing quietly.

Other options:

| Option | Effect |
|--------|--------|
| `-DSANITIZE=address,undefined` | Build with the named sanitizers. Also sets `halt_on_error` for every registered test, so a TSan/UBSan finding fails the run instead of printing a warning into a green log. |
| `-DWERROR=ON` | Treat warnings as errors. |
| `-DBUILD_FUZZERS=ON` | AMF fuzzers; `fuzz_run` is registered as a ctest. |

### Throughput benchmark

`bench_rtmp_throughput` spawns a server and loads it over RTMP. It is a manual
benchmark, not a test:

```sh
# 1 publisher -> 32 subscribers on ONE stream (fan-out), 14 server threads
./build/bench_rtmp_throughput 16384 12 60000 32 14 27000 512 0 1
```

The trailing `1` selects fan-out; without it the run is N independent
publisher/subscriber pairs instead. On Linux it also reports how many cores the
server burned versus the whole box, which is what shows where it saturates.

---

## Logging

The server writes rotating log files to `--log-path` (default: current
directory). Verbosity is controlled with `--log-level` (higher is more verbose).
