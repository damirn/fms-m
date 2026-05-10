# F Media Server (fms-m)

A lightweight, multi-threaded **RTMP / RTMFP media server** for live audio/video
streaming. It accepts published live streams (as from OBS, ffmpeg, or Flash-era
encoders), relays them to any number of subscribers in real time, and can record
incoming streams to FLV files on disk.

The server speaks the full family of Adobe streaming protocols — plain RTMP,
encrypted RTMPE, HTTP-tunnelled RTMPT, and UDP-based RTMFP — over a
`io_context`-per-core Boost.Asio engine, so a single process scales across all
available CPU cores.

- **Language / build:** C++23, CMake, Boost, OpenSSL 3, Speex
- **Platforms:** macOS (Apple clang) and Linux (GCC/clang)
- **Version:** 0.28.1

---

## Features

- **Live relay** — one publisher fans out to many subscribers with per-client
  queueing and QoS notifications.
- **Recording** — publish a stream as type `record` and the server writes it to
  `<output-folder>/<stream>.flv`.
- **Multiple transports** for the same content:
  - **RTMP** — plain TCP (default port `1935`)
  - **RTMPE** — encrypted RTMP (RC4 + Diffie-Hellman handshake), same port
  - **RTMPT** — RTMP tunnelled over HTTP (default port `80`)
  - **RTMFP** — Adobe's UDP real-time protocol (default port `1935/udp`)
- **Codec pass-through** for H.264 video and AAC audio, plus built-in Speex and
  G.711 audio support used by the call application.
- **Admin application** — a password-protected control app exposing live
  application/client/stream statistics and the ability to disconnect clients.
- **Pluggable authentication** — an optional shared-library auth plugin can gate
  who is allowed to connect and publish.
- **Multi-threaded** — a pool of I/O threads, one `io_context` per thread, with
  each connection pinned to a single thread.

---

## Applications

RTMP organises streams under a named *application*. A client connects to
`rtmp://host/<app>/<stream>`. Three applications are registered:

| App name     | Purpose                                                                 |
|--------------|-------------------------------------------------------------------------|
| `bcast`      | General live broadcast: publish, play, and record.                      |
| `video_call` | Two-way video-call variant of `bcast` (adds call signalling / mixing).  |
| `admin`      | Administrative control and monitoring (requires a password file).       |

The publish/play/record URL for the broadcast app is
`rtmp://host:1935/bcast/<streamName>`.

---

## Building

### Dependencies

- A C++23 compiler (Apple clang 16+, or GCC 12+)
- CMake ≥ 3.16
- **Boost** — `date_time`, `log`, `log_setup`, `program_options`, `system`,
  `thread` (macOS builds pin Boost **1.76**; Linux uses the system Boost)
- **OpenSSL 3** — `libcrypto` (the **legacy provider** must be available for
  RTMPE, which uses RC4)
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
| `-t`  | `--threads <n>`                   | `1`       | Number of I/O threads (one `io_context` per thread).   |
| `-o`  | `--output-folder <path>`          | `.`       | Destination folder for recorded `.flv` files.          |
| `-c`  | `--config-file <path>`            |           | Optional config file (same keys as CLI options).       |
| `-P`  | `--log-path <path>`               | `.`       | Directory for the rotating server log files.           |
| `-f`  | `--log-file <name>`               | *(auto)*  | Explicit log file name.                                |
| `-l`  | `--log-level <n>`                 | `3`       | Log verbosity level.                                   |
| `-a`  | `--auth-plugin <path>`            | *(none)*  | Path to an authentication plugin shared library.       |
| `-F`  | `--password-file <path>`          | `./passwd`| Password file used by the `admin` application.         |
| `-A`  | `--admin-data-keep-time <sec>`    | `600`     | How long admin statistics are retained, in seconds.    |
| `-H`  | `--helper-app <path>`             | *(none)*  | External helper application.                           |
| `-q`  | `--quality <0..10>`               | `6`       | Speex encoder quality.                                 |
| `-e`  | `--max-audio-frames <n>`          | `2`       | Max audio frames queued per client.                    |
| `-E`  | `--max-audio-frames-high-latency` | `10`      | Max audio frames queued per client in high-latency mode.|
| `-n`  | `--notify-threshold <ms>`         | `2000`    | Delay (ms) after which a client is warned/throttled.   |
| `-r`  | `--terminate-threshold <ms>`      | `3000`    | Delay (ms) after which a lagging client is dropped.    |

Options may also be supplied through a config file (`--config-file`) using the
long option names as keys.

---

## Usage examples

The examples use [`ffmpeg`](https://ffmpeg.org/) to publish/play plain RTMP and
[`rtmpdump`](https://rtmpdump.mplayerhq.hu/) for encrypted RTMPE.

### Publish a live stream (RTMP)

```sh
ffmpeg -re -i input.mp4 -c copy -f flv rtmp://127.0.0.1:1935/bcast/mystream
```

### Play a live stream (RTMP)

```sh
ffmpeg -i rtmp://127.0.0.1:1935/bcast/mystream -c copy out.flv
# or watch it live:
ffplay rtmp://127.0.0.1:1935/bcast/mystream
```

### Play an encrypted stream (RTMPE)

```sh
rtmpdump -r "rtmpe://127.0.0.1:1935/bcast/mystream" --live -o out.flv
```

RTMPE reuses the RTMP port — the client selects encryption via the `rtmpe://`
scheme. The server negotiates the RC4/Diffie-Hellman handshake automatically.

### Play over RTMPT (HTTP tunnel)

```sh
ffmpeg -i rtmpt://127.0.0.1:8080/bcast/mystream -c copy out.flv
```

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

## Admin application & authentication

- **Admin app** — connect to the `admin` application to query live statistics
  (applications, clients, streams, queue stats) and to disconnect clients.
  Access is authenticated against the file given by `--password-file` (default
  `./passwd`). Each line is `user:sha256hash`, optionally salted as
  `user:salt$sha256(salt+password)`.
- **Auth plugin** — `--auth-plugin <lib>` loads an external shared library that
  can approve or reject connections and publishes, for integrating with your own
  user directory or token system.

---

## Logging

The server writes rotating log files to `--log-path` (default: current
directory). Verbosity is controlled with `--log-level` (higher is more verbose).

---

## Project layout

| Path                          | Contents                                             |
|-------------------------------|------------------------------------------------------|
| `main.cpp`, `server.*`        | Process entry point and acceptor/listener setup.     |
| `rtmp_connection.*`, `basic_rtmp_connection.*` | RTMP/RTMPE connection + handshake state machine. |
| `rtmp_protocol.*`, `rtmp_message.*`, `rtmp_header.*` | RTMP chunking and message (de)serialization. |
| `amf0.*`, `amf3.*`            | AMF0 / AMF3 command and value encoding.              |
| `crypto.*`, `dh.*`, `evp_dh.*`| RTMPE crypto: RC4, HMAC-SHA256, Diffie-Hellman.      |
| `http_connection.*`, `rtmpt_*`| RTMPT (HTTP-tunnelled RTMP).                          |
| `rtmfp/`                      | RTMFP (UDP) service, sessions, and flows.            |
| `video_bcast_application.*`   | The `bcast` application (publish/play/record).       |
| `video_call_application.*`    | The `video_call` application.                        |
| `admin_application.*`         | The `admin` application.                             |
| `flv_writer.*`, `flv_reader.*`| FLV file writing/reading.                            |
| `io_service_pool.*`           | The `io_context`-per-thread engine.                  |
| `stream_array.h`, `dynamic_array.h` | The network read/write buffer.                 |
