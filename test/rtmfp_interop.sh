#!/usr/bin/env bash
#
# RTMFP interop test: publish into fms-m over RTMFP using Michael Thornburgh's
# reference implementation (zenomt/rtmfp-cpp `tcpublish`, the RFC 7016 author),
# then pull the stream back over RTMP and assert media arrived. This exercises
# the full RTMFP path against an independent implementation: handshake (HMAC
# cookie, keying), session, per-packet framing, and flow reassembly.
#
# Opt-in / not a unit test: it needs a running server, ffmpeg, and a checkout of
# rtmfp-cpp. It SKIPs (exit 0) if those aren't available.
#
#   env vars:
#     FMS_BIN      path to the fms-m binary            (default: build/fms-m)
#     RTMFP_CPP    path to a zenomt/rtmfp-cpp checkout  (default: ../rtmfp-cpp, ../../rtmfp-cpp)
#     OPENSSL_DIR  openssl prefix for building tcpublish (default: /opt/homebrew/opt/openssl@3)
#     TCPUB_FLAGS  extra tcpublish flags. Empty = STRICT (require session HMAC +
#                  sequence numbers, RFC 7016). Set "-H -S" to relax them.
#
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

skip() { echo "SKIP: $*"; exit 0; }
fail() { echo "FAIL: $*"; cleanup; exit 1; }

FMS_BIN="${FMS_BIN:-$root/build/fms-m}"
OPENSSL_DIR="${OPENSSL_DIR:-/opt/homebrew/opt/openssl@3}"
TCPUB_FLAGS="${TCPUB_FLAGS:-}"   # empty = strict; "-H -S" to relax

command -v ffmpeg  >/dev/null 2>&1 || skip "ffmpeg not found"
command -v ffprobe >/dev/null 2>&1 || skip "ffprobe not found"
command -v lsof    >/dev/null 2>&1 || skip "lsof not found"
[ -x "$FMS_BIN" ] || skip "fms-m not built at $FMS_BIN (configure+build first)"

# locate rtmfp-cpp
RTMFP_CPP="${RTMFP_CPP:-}"
if [ -z "$RTMFP_CPP" ]; then
  for d in "$root/../rtmfp-cpp" "$root/../../rtmfp-cpp"; do
    [ -d "$d/test" ] && RTMFP_CPP="$d" && break
  done
fi
[ -n "$RTMFP_CPP" ] && [ -d "$RTMFP_CPP/test" ] || skip "rtmfp-cpp not found (set RTMFP_CPP=...)"

TCPUBLISH="$RTMFP_CPP/test/tcpublish"
if [ ! -x "$TCPUBLISH" ]; then
  echo "building tcpublish (OPENSSL_DIR=$OPENSSL_DIR) ..."
  make -C "$RTMFP_CPP/test" OPENSSL_DIR="$OPENSSL_DIR" tcpublish >/dev/null 2>&1 \
    || skip "could not build tcpublish (check OPENSSL_DIR)"
fi
[ -x "$TCPUBLISH" ] || skip "tcpublish not available"

work="$(mktemp -d)"
FMS_PID=""; PUB_PID=""
cleanup() {
  [ -n "$PUB_PID" ] && kill "$PUB_PID" 2>/dev/null
  [ -n "$FMS_PID" ] && kill -TERM "$FMS_PID" 2>/dev/null
  pkill -f "$TCPUBLISH" 2>/dev/null
  rm -rf "$work"
}
trap cleanup EXIT

# macOS ICU runtime shim (boost_log links libicudata.74); harmless elsewhere.
export DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-}:/opt/homebrew/opt/icu4c@74/lib"

# 1. a short H.264+AAC FLV to publish
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i "testsrc2=size=320x240:rate=25" \
  -f lavfi -i "sine=frequency=440:sample_rate=44100" \
  -c:v libx264 -preset ultrafast -tune zerolatency -g 25 -pix_fmt yuv420p \
  -c:a aac -ar 44100 -t 8 -y "$work/src.flv" \
  || fail "could not generate test FLV"

# 2. start fms-m (RTMP tcp/1935 + RTMFP udp/1935); -P puts its log files in $work
"$FMS_BIN" -P "$work" >"$work/fms.stdout" 2>&1 &
FMS_PID=$!
for _ in $(seq 1 40); do lsof -nP -iUDP:1935 2>/dev/null | grep -q "$FMS_PID" && break; sleep 0.25; done
lsof -nP -iUDP:1935 2>/dev/null | grep -q "$FMS_PID" || fail "fms-m did not bind UDP:1935"

# 3. publish into fms-m over RTMFP (loop so the RTMP puller can catch it)
echo "publishing over RTMFP (flags: '${TCPUB_FLAGS:-<strict>}') ..."
# -v so tcpublish prints its status; -L so it keeps publishing while we pull.
# shellcheck disable=SC2086
"$TCPUBLISH" -v $TCPUB_FLAGS -L rtmfp://localhost/bcast#interop "$work/src.flv" >"$work/pub.log" 2>&1 &
PUB_PID=$!

# 4. give the RTMFP session ~4s to establish + publish. A successful -L publisher
# keeps running; a failed handshake/connect exits. If it exits early, that's the
# missing-session-HMAC/seq case (tcpublish requires them unless -H -S).
for _ in $(seq 1 8); do ps -p "$PUB_PID" >/dev/null 2>&1 || break; sleep 0.5; done
if ! ps -p "$PUB_PID" >/dev/null 2>&1; then
  echo "--- tcpublish log ---"; tail -25 "$work/pub.log"
  fail "tcpublish exited before publishing. If it shows Connect.Failed, fms-m is missing the per-packet session HMAC/sequence numbers -- try TCPUB_FLAGS='-H -S'."
fi

# 5. pull the stream back over RTMP and assert media. -rw_timeout bounds the read
# so we don't hang if no media crossed the bridge.
ffmpeg -hide_banner -v error -rw_timeout 8000000 -i rtmp://localhost:1935/bcast/interop -t 4 -c copy -y "$work/out.flv" 2>/dev/null
[ -s "$work/out.flv" ] || fail "no RTMP capture of the RTMFP-published stream"
vframes="$(ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames -of csv=p=0 "$work/out.flv" 2>/dev/null)"
aframes="$(ffprobe -v error -count_frames -select_streams a -show_entries stream=nb_read_frames -of csv=p=0 "$work/out.flv" 2>/dev/null)"
codecs="$(ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$work/out.flv" 2>/dev/null | tr '\n' ' ')"
derr="$(ffmpeg -hide_banner -v error -i "$work/out.flv" -f null - 2>&1 | head -1)"

echo "captured: codecs=[$codecs] video_frames=${vframes:-0} audio_frames=${aframes:-0} decode_err=[${derr:-none}]"
[ "${vframes:-0}" -ge 10 ] 2>/dev/null || fail "too few video frames (${vframes:-0}) crossed the RTMFP->RTMP bridge"
[ -z "$derr" ] || fail "decode errors in the captured stream"

echo "PASS: RTMFP publish (reference tcpublish) -> fms-m -> RTMP pull, media intact"
exit 0
