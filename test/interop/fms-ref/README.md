# FMS 4.5 reference oracle

A stock **Adobe Flash Media Server 4.5.0 r297** (2011) in a container, used as
ground truth when we need to know *exactly* what real FMS does on the wire —
e.g. the `BufferEmpty(31)` / `BufferReady(32)` sequencing that `ce436cb`
reproduces. When a protocol question can't be settled from docs or reverse
engineering, capture it here and match fms-m to it.

## Why a container

The binaries are linux **x86-64** and need only `GLIBC <= 2.4` + **NSPR** at
runtime (OpenSSL 1.0.0, expat and the adbe libs are bundled). glibc is backward
compatible, so a modern `debian:bullseye` runs them under amd64 emulation. The
only tricks: install `libnspr4`, and symlink the legacy `libcap.so.1` (v1) to the
installed `libcap.so.2` — `fmsedge` uses only the 5 process-cap functions that
are ABI-identical across the v1→v2 soname bump.

## The binaries are NOT in the repo

FMS 4.5 is Adobe-proprietary. Drop the tarball here before building:

```
cp /path/to/FlashMediaServer4.5_x64.tar.gz test/interop/fms-ref/
```

(`.gitignore` keeps the tarball, the extracted tree, and capture artifacts out of
git.)

## Build & run

```sh
cd test/interop/fms-ref
docker build  --platform linux/amd64 -t fms45 .
docker run -d --platform linux/amd64 --name fms45 -p 11935:1935 fms45
docker logs fms45            # expect "Server started"; fmsedge LISTEN on :1935
```

RTMP is then on host `:11935` with the stock `live` and `vod` apps. Stop with
`docker rm -f fms45`.

## Capturing behaviour

Publish with ffmpeg, play with rtmpdump in debug (`-z`) so every user-control is
logged as `HandleCtrl, received ctrl. type: N`. **Live playback needs rtmpdump
`-v` (--live)** — without it rtmpdump looks for a recorded `<name>.flv` and gets
`NetStream.Play.StreamNotFound`.

```sh
# publisher-active, then play:
ffmpeg -re -f lavfi -i testsrc2 -f lavfi -i sine -t 12 \
       -c:v libx264 -preset ultrafast -c:a aac -f flv rtmp://localhost:11935/live/x &
rtmpdump -z -v -r rtmp://localhost:11935/live/x -o out.flv
grep -E "received ctrl|NetStream.Play" out.log
```

FMS's own logs are inside the container at `/opt/fms/logs/` (`access.00.log` is
the most useful — one line per connect/publish/play/unpublish with status codes).

## Captured contract (reference)

- Live play start: `StreamBegin(0)` → `Play.Reset` → `Play.Start` →
  `BufferEmpty(31)` → `BufferReady(32)` once data flows. 31/32 then toggle
  continuously at the live edge (advisory; fms-m emits only the empty→full edge).
- Subscriber that connects before any publisher: `BufferEmpty(31)` stands alone
  until a publisher appears, then `BufferReady(32)` + `Play.PublishNotify`.
- Publisher unpublish: `NetStream.Play.UnpublishNotify` + drain to `BufferEmpty`;
  the subscriber stays connected to resume on republish (FMS does **not** send
  `StreamEOF(1)` here).
