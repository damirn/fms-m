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
#   RTMFP play/publish (rtmfp-cpp)          -- strict crypto, no -H -S relaxation;
#                                              tcconn logs each received A/V frame
#   RTMFP -> RTMP bridge (tcpublish -> rtmpdump) -- ingest + cross-protocol fan-out
# Deliberately NOT covered locally:
#   RTMPE  -- rtmpdump bus-errors on the encrypted handshake on this platform
#
# Usage: interop.sh [path-to-fms-m]   (default: build-test/fms-m)
# Requires: rtmpdump, ffmpeg, ffprobe on PATH.
# Optional: rtmfp-cpp's built tcpublish/tcconn (set RTMFP_CPP to its test/ dir);
#           RTMFP cases skip cleanly when they are absent.
# ---------------------------------------------------------------------------
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FMS="${1:-$ROOT/build-test/fms-m}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/fms-interop.XXXXXX")"
RTMP_PORT=27000
RTMPT_PORT=27001
RTMFP_PORT=27002

# rtmfp-cpp reference clients (optional). tcpublish = publish, tcconn = play.
# Built with: make tcpublish tcconn OPENSSL_DIR=/opt/homebrew/opt/openssl@3
RTMFP_CPP="${RTMFP_CPP:-/Users/damir/Documents/work/playground/rtmfp-cpp/test}"
TCPUBLISH="$RTMFP_CPP/tcpublish"
TCCONN="$RTMFP_CPP/tcconn"

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
cleanup() { [[ -n "$SRV" ]] && kill "$SRV" 2>/dev/null; pkill -f "$FMS" 2>/dev/null; pkill -f tcpublish 2>/dev/null; pkill -f tcconn 2>/dev/null; wait 2>/dev/null; rm -rf "$WORK"; }
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

# ---- RTMFP (rtmfp-cpp) helpers -----------------------------------------------
# The stream name is the rtmfp-uri #fragment (NOT the path); the path is the app.
# So bcast#<name> mirrors RTMP's bcast/<name>. Strict crypto: no -H -S needed.
have_rtmfp() { [[ -x "$TCPUBLISH" && -x "$TCCONN" ]]; }

# publish_rtmfp <name> <flv> -- background tcpublish (paces the FLV in real time);
# echoes its pid.
publish_rtmfp() {
	"$TCPUBLISH" -4 "rtmfp://127.0.0.1:$RTMFP_PORT/bcast#$1" "$2" >"$WORK/rtmfp_pub_$1.log" 2>&1 &
	echo $!
}

# play_rtmfp <name> <max_seconds> -- tcconn in verbose mode (logs each received
# A/V frame as "stream onVideo/onAudio ..."), killed after max_seconds.
play_rtmfp() {
	local name="$1" secs="$2"
	"$TCCONN" -4 -v -v "rtmfp://127.0.0.1:$RTMFP_PORT/bcast#$name" >"$WORK/rtmfp_play_$name.log" 2>&1 &
	local pid=$!
	for _ in $(seq 1 $((secs*4))); do kill -0 "$pid" 2>/dev/null || return 0; sleep 0.25; done
	kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; return 0
}

# saw_media <tcconn-log> -- did tcconn actually receive audio or video frames?
saw_media() { grep -qE "stream on(Video|Audio) " "$1"; }

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
# FMS 4.5 parity: Play.Start -> BufferEmpty(31) (buffer starts empty) -> BufferReady
# (32) once media flows. Verified against stock FMS in a container.
saw_ctrl "$WORK/live.log" 31           && ok "live: BufferEmpty(31) after Play.Start" || bad "live: BufferEmpty(31)"
saw_ctrl "$WORK/live.log" 32           && ok "live: BufferReady(32) once media flows" || bad "live: BufferReady(32)"

# --- Case 2: RTMP VOD (served .flv), with SetBufferLength fast pull -----------
# rtmpdump sends SetBufferLength with a huge buffer (BUFX); the server honours it
# and streams the whole file at once instead of pacing in real time. A 6s clip that
# arrives in well under its own duration proves the buffer is acted on (real-time
# pacing would take ~6s). EOF user-control events must still be delivered.
echo "[2] RTMP VOD: fast pull (SetBufferLength/BUFX) a saved .flv to EOF"
make_source "$WORK/clip.flv" 6
vod_t0=$SECONDS
play_rtmpdump "rtmp://127.0.0.1:$RTMP_PORT/bcast/clip" "$WORK/vod.flv" 12
vod_secs=$((SECONDS - vod_t0))
has_av "$WORK/vod.flv"                  && ok "vod: valid A/V received" || bad "vod: media"
[[ "$vod_secs" -lt 3 ]]                 && ok "vod: 6s clip pulled in ${vod_secs}s (buffer honoured)" || bad "vod: fast pull (${vod_secs}s, expected <3s)"
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

# --- Case 4/5: RTMFP (rtmfp-cpp reference clients, strict crypto) -------------
if have_rtmfp; then
	echo "[4] RTMFP live: tcpublish -> tcconn (strict crypto, no -H -S)"
	make_source "$WORK/rtmfp.flv" 6
	PUB=$(publish_rtmfp live "$WORK/rtmfp.flv")
	sleep 1.5
	play_rtmfp live 3
	kill "$PUB" 2>/dev/null
	saw_media "$WORK/rtmfp_play_live.log"  && ok "rtmfp: A/V received over RTMFP" || bad "rtmfp: media"

	echo "[5] RTMFP->RTMP bridge: tcpublish -> rtmpdump"
	PUB=$(publish_rtmfp bridge "$WORK/rtmfp.flv")
	sleep 1.5
	play_rtmpdump "rtmp://127.0.0.1:$RTMP_PORT/bcast/bridge" "$WORK/bridge.flv" 4
	kill "$PUB" 2>/dev/null
	has_av "$WORK/bridge.flv"               && ok "rtmfp->rtmp: valid A/V bridged" || bad "rtmfp->rtmp: media"
	saw_ctrl "$WORK/bridge.log" 0          && ok "rtmfp->rtmp: StreamBegin(0) sent+consumed" || bad "rtmfp->rtmp: StreamBegin"
else
	skip "RTMFP: rtmfp-cpp tcpublish/tcconn not built (set RTMFP_CPP to its test/ dir)"
fi

# --- Case 6: app resolution vs a leading-slash "app" (librtmfp shape) ---------
# The connect "app" is a URL path. librtmfp sends it raw ("/bcast"); rtmfp-cpp
# strips the leading '/' for us, which is the only reason plain matching worked.
# rtmpdump's -a lets us send the un-normalized form over RTMP (app resolution is
# protocol-agnostic) -- a stand-in for librtmfp until it's available locally.
echo "[6] app resolution: leading-slash 'app' (librtmfp shape) must connect"
rtmpdump -z -r "rtmp://127.0.0.1:$RTMP_PORT" -a "/bcast" -y "clip" -o "$WORK/slash.flv" >"$WORK/slash.log" 2>&1
if grep -q "NetConnection.Connect.Success" "$WORK/slash.log" && has_av "$WORK/slash.flv"; then
	ok "app: '/bcast' resolves + plays (no spurious InvalidApp)"
else
	bad "app: '/bcast' rejected (InvalidApp regression)"
fi

# --- Case 7: live unpublish (FMS parity: UnpublishNotify + drain, no StreamEOF) --
# When a live publisher stops, FMS sends the subscriber Play.UnpublishNotify and a
# BufferEmpty drain -- NOT StreamEOF(1) (that is a VOD end-of-file signal). rtmpdump
# closes cleanly on UnpublishNotify. Verified against a stock FMS 4.5 container.
echo "[7] live unpublish: publisher stops mid-play -> UnpublishNotify, no StreamEOF"
PUB=$(publish_live unp 3)          # short-lived publisher; ends on its own
sleep 0.5
play_rtmpdump "rtmp://127.0.0.1:$RTMP_PORT/bcast/unp" "$WORK/unp.flv" 8 -v
kill "$PUB" 2>/dev/null
has_av "$WORK/unp.flv"                          && ok "unpublish: media received before unpublish" || bad "unpublish: media"
grep -q "UnpublishNotify" "$WORK/unp.log"      && ok "unpublish: Play.UnpublishNotify sent" || bad "unpublish: UnpublishNotify missing"
! saw_ctrl "$WORK/unp.log" 1                    && ok "unpublish: no StreamEOF(1) on live (FMS parity)" || bad "unpublish: spurious StreamEOF(1)"

# --- documented gaps ---------------------------------------------------------
skip "RTMPE: rtmpdump bus-errors on the encrypted handshake on this platform"
echo
echo "=== results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[[ $FAIL -eq 0 ]]
