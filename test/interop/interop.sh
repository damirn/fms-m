#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# fms-m interop test matrix (local: RTMP / RTMPT via rtmpdump + ffmpeg).
#
# Spins up the server, drives it with real reference clients, and asserts on
# both the media (ffprobe) AND the RTMP user-control (ping) events the client
# receives -- rtmpdump's debug (-z) logs each as "HandleCtrl, received ctrl.
# type: N" (0=StreamBegin 1=StreamEOF 2=StreamDry 3=SetBufferLength(sent by
# client) 4=StreamIsRecorded 31=BufferEmpty 32=BufferReady). That is how we
# confirm each event is both SENT by the server and CONSUMED by the client.
#
# Coverage here is what works locally:
#   RTMP  play/publish (rtmpdump + ffmpeg)  -- events observable
#   RTMPT play          (ffmpeg)            -- media only (ffmpeg has no ping log)
# Deliberately NOT covered locally:
#   RTMPE  -- rtmpdump bus-errors on the encrypted handshake on this platform
#   RTMFP  -- no local reference client (rtmfp-cpp lives on the Linux box)
#
# Usage: interop.sh [path-to-fms-m]   (default: build-test/fms-m)
# Requires: rtmpdump, ffmpeg, ffprobe on PATH.
# ---------------------------------------------------------------------------
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FMS="${1:-$ROOT/build-test/fms-m}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/fms-interop.XXXXXX")"
RTMP_PORT=27000
RTMPT_PORT=27001
RTMFP_PORT=27002

# macOS: Boost.Log needs icu4c@74 at its old keg path (see docs / build notes).
if [[ "$(uname)" == "Darwin" && -d /opt/homebrew/opt/icu4c@74/lib ]]; then
	export DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/opt/icu4c@74/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
fi

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS: $*"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL: $*"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $*"; SKIP=$((SKIP+1)); }

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1"; exit 2; }; }
need rtmpdump; need ffmpeg; need ffprobe
[[ -x "$FMS" ]] || { echo "fms-m not found/executable: $FMS"; exit 2; }

SRV=
cleanup() { [[ -n "$SRV" ]] && kill "$SRV" 2>/dev/null; pkill -f "$FMS" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

start_server() {
	mkdir -p "$WORK/logs"
	"$FMS" -R "$RTMP_PORT" -T "$RTMPT_PORT" -K "$RTMFP_PORT" -o "$WORK" -P "$WORK/logs" -t 4 \
		>"$WORK/server.out" 2>&1 &
	SRV=$!
	for _ in $(seq 1 40); do
		lsof -nP -iTCP:"$RTMP_PORT" -sTCP:LISTEN >/dev/null 2>&1 && return 0
		sleep 0.25
	done
	echo "server failed to listen on $RTMP_PORT"; return 1
}

# make_source <path> <seconds> -- a deterministic H.264+AAC FLV
make_source() {
	ffmpeg -hide_banner -loglevel error -y \
		-f lavfi -i "testsrc2=size=320x240:rate=15" \
		-f lavfi -i "sine=frequency=440" -t "$2" \
		-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -f flv "$1"
}

# publish_live <name> <seconds> -- background ffmpeg publisher; echoes its pid
publish_live() {
	ffmpeg -hide_banner -loglevel error -re \
		-f lavfi -i "testsrc2=size=320x240:rate=15" \
		-f lavfi -i "sine=frequency=440" -t "$2" \
		-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac \
		-f flv "rtmp://127.0.0.1:$RTMP_PORT/bcast/$1" >"$WORK/pub_$1.log" 2>&1 &
	echo $!
}

# play_rtmpdump <url> <out.flv> <max_seconds> <extra rtmpdump args...>
# Runs rtmpdump in debug mode, killing it after max_seconds if it hasn't exited.
play_rtmpdump() {
	local url="$1" out="$2" secs="$3"; shift 3
	rtmpdump -z "$@" -r "$url" -o "$out" >"${out%.flv}.log" 2>&1 &
	local rd=$!
	for _ in $(seq 1 $((secs*4))); do kill -0 "$rd" 2>/dev/null || return 0; sleep 0.25; done
	kill "$rd" 2>/dev/null; wait "$rd" 2>/dev/null; return 0
}

has_av() {   # ffprobe: file has both a video and an audio stream
	local f="$1"
	[[ -s "$f" ]] || return 1
	local t; t="$(ffprobe -hide_banner -v error -show_entries stream=codec_type -of csv=p=0 "$f" 2>/dev/null | tr '\n' ' ')"
	[[ "$t" == *video* && "$t" == *audio* ]]
}

# saw_ctrl <log> <type-number> -- did rtmpdump log receiving that user-control type?
saw_ctrl() { grep -qE "received ctrl\. type: $2," "$1"; }

echo "=== fms-m interop matrix ==="
echo "server: $FMS   work: $WORK"
start_server || exit 1

# --- Case 1: RTMP live (publish + play) ---------------------------------------
echo "[1] RTMP live: ffmpeg publish -> rtmpdump play"
PUB=$(publish_live live 8)
sleep 1.5
play_rtmpdump "rtmp://127.0.0.1:$RTMP_PORT/bcast/live" "$WORK/live.flv" 4 -v
kill "$PUB" 2>/dev/null
has_av "$WORK/live.flv"                 && ok "live: valid A/V received" || bad "live: media"
saw_ctrl "$WORK/live.log" 0             && ok "live: StreamBegin(0) sent+consumed" || bad "live: StreamBegin"

# --- Case 2: RTMP VOD (served .flv) -------------------------------------------
echo "[2] RTMP VOD: play a saved .flv to EOF"
make_source "$WORK/clip.flv" 3
play_rtmpdump "rtmp://127.0.0.1:$RTMP_PORT/bcast/clip" "$WORK/vod.flv" 8
has_av "$WORK/vod.flv"                  && ok "vod: valid A/V received" || bad "vod: media"
saw_ctrl "$WORK/vod.log" 0             && ok "vod: StreamBegin(0)" || bad "vod: StreamBegin"
saw_ctrl "$WORK/vod.log" 4             && ok "vod: StreamIsRecorded(4) sent+consumed" || bad "vod: StreamIsRecorded"
saw_ctrl "$WORK/vod.log" 1             && ok "vod: StreamEOF(1) at end sent+consumed" || bad "vod: StreamEOF"

# --- Case 3: RTMPT live (ffmpeg play; media only) ----------------------------
echo "[3] RTMPT tunnel: ffmpeg publish(rtmp) -> ffmpeg play(rtmpt)"
PUB=$(publish_live tun 8)
sleep 1.5
ffmpeg -hide_banner -loglevel error -y -i "rtmpt://127.0.0.1:$RTMPT_PORT/bcast/tun" -t 2 -c copy -f flv "$WORK/tun.flv" >"$WORK/tun_play.log" 2>&1
kill "$PUB" 2>/dev/null
has_av "$WORK/tun.flv"                  && ok "rtmpt: valid A/V over the HTTP tunnel" || bad "rtmpt: media"

# --- documented gaps ---------------------------------------------------------
skip "RTMPE: rtmpdump bus-errors on the encrypted handshake on this platform"
skip "RTMFP: no local reference client (rtmfp-cpp is on the Linux box)"
echo
echo "=== results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[[ $FAIL -eq 0 ]]
