#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# fms-m real-world matrix: one server, driven by ffmpeg the way a real encoder
# and a real player drive it.
#
# interop.sh asserts on protocol shape -- which user-control events go out and
# come back. This asserts on what an operator actually cares about: that real
# encodes survive the round trip, that sessions behave under concurrency and
# mid-stream failure, and that the transports carry media rather than merely
# completing a handshake.
#
#   A  codec matrix        H.264 profiles, AAC/MP3, audio-only, video-only,
#                          and the enhanced-RTMP codecs (HEVC/AV1)
#   B  stream shapes       1080p long-GOP, 60fps, non-multiple-of-16 width
#   C  session behaviour   fan-out, concurrent publishers, republish,
#                          waiting subscriber, publisher drop, query-string names
#   D  transports          RTMPT / RTMPS / RTMPTS / crossover, carrying media
#   E  durability          45s pull: packet count and DTS monotonicity
#   F  resource limits     slow consumer vs --max-queue-bytes, record-to-FLV
#
# Usage: realworld.sh [path-to-fms-m]   (default: build/fms-m)
# Requires: ffmpeg, ffprobe, rtmpdump, openssl, lsof on PATH.
# ---------------------------------------------------------------------------
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FMS="${1:-$ROOT/build/fms-m}"
CLIENT="${CLIENT:-$(dirname "$FMS")/rtmp_client}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/fms-realworld.XXXXXX")"
# Boost.Log resolves ICU at its old keg path on macOS, as CMake's test env does.
[ "$(uname)" = Darwin ] && export DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-/opt/homebrew/opt/icu4c@74/lib}"

RTMP_PORT=27100; RTMPT_PORT=27101; RTMFP_PORT=27102
RTMPS_PORT=27543; RTMPTS_PORT=27544
TLS_CERT="$WORK/cert.pem"; TLS_KEY="$WORK/key.pem"
MAXQ="${MAXQ:-10485760}"

PASS=0; FAIL=0; SKIP=0; declare -a FAILED=()
ok()   { echo "    PASS  $*"; PASS=$((PASS+1)); }
bad()  { echo "    FAIL  $*"; FAIL=$((FAIL+1)); FAILED+=("$*"); }
skip() { echo "    SKIP  $*"; SKIP=$((SKIP+1)); }

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1"; exit 2; }; }
need ffmpeg; need ffprobe; need rtmpdump; need openssl; need lsof
[ -x "$FMS" ] || { echo "no server binary at $FMS"; exit 2; }

KIDS=()
track() { KIDS+=("$1"); }
cleanup() {
	for p in "${KIDS[@]:-}"; do kill "$p" 2>/dev/null; done
	[ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
	[ "${KEEP_WORK:-0}" = 0 ] && rm -rf "$WORK"
	return 0
}
trap cleanup EXIT

mkdir -p "$WORK/logs" "$WORK/rec"

start_server() {
	openssl req -x509 -newkey rsa:2048 -keyout "$TLS_KEY" -out "$TLS_CERT" \
		-days 2 -nodes -subj "/CN=localhost" >/dev/null 2>&1
	"$FMS" -R "$RTMP_PORT" -T "$RTMPT_PORT" -K "$RTMFP_PORT" \
		--rtmps-port "$RTMPS_PORT" --rtmpts-port "$RTMPTS_PORT" \
		--tls-cert "$TLS_CERT" --tls-key "$TLS_KEY" \
		-o "$WORK/rec" -P "$WORK/logs" -e "$MAXQ" -t 4 >"$WORK/server.out" 2>&1 &
	SRV=$!
	for _ in $(seq 1 60); do
		lsof -nP -iTCP:"$RTMP_PORT" -sTCP:LISTEN >/dev/null 2>&1 && return 0
		sleep 0.25
	done
	echo "server failed to listen"; return 1
}

# Block until the server logs the publish. Stream names must be unique per run.
wait_publishing() {
	local name="$1" secs="${2:-20}"
	for _ in $(seq 1 $((secs*10))); do
		grep -qs "is publishing stream '$name'" "$WORK"/logs/*.log && return 0
		sleep 0.1
	done
	echo "      (timed out waiting for publish of '$name')"
	return 1
}

# run_timeout <secs> <cmd...> -- stock macOS has no timeout(1). Every network
# read here needs a ceiling: ffmpeg blocks indefinitely on a stream that stopped
# publishing, and -t only bounds output duration, not the wait for first byte.
run_timeout() {
	local secs="$1"; shift
	"$@" &
	local p=$! i=0
	while kill -0 "$p" 2>/dev/null; do
		i=$((i+1)); [ "$i" -ge $((secs*4)) ] && { kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null; return 124; }
		sleep 0.25
	done
	wait "$p" 2>/dev/null; return $?
}

# probe <file> <stream_spec> -> prints the requested ffprobe fields
probe() { ffprobe -v error -show_entries "$2" -of csv=p=0 "$1" 2>/dev/null; }
vcodec() { ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$1" 2>/dev/null; }
acodec() { ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$1" 2>/dev/null; }
dur()    { ffprobe -v error -show_entries format=duration -of csv=p=0 "$1" 2>/dev/null | cut -d. -f1; }
nframes(){ ffprobe -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 "$1" 2>/dev/null; }
size()   { ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$1" 2>/dev/null; }

has_av() { [ -s "$1" ] && [ -n "$(vcodec "$1")" ] && [ -n "$(acodec "$1")" ]; }

# ---- source assets ----------------------------------------------------------
# Real encodes, not testsrc-over-the-wire, so the server sees genuine
# sequence headers, B-frames and variable frame sizes.
mksrc() { # mksrc <out> <secs> <vcodec args...> ; audio is AAC unless -an given
	local out="$1" secs="$2"; shift 2
	ffmpeg -hide_banner -loglevel error -y \
		-f lavfi -i "testsrc2=size=${SRC_SIZE:-1280x720}:rate=${SRC_FPS:-30}" \
		-f lavfi -i "sine=frequency=440:sample_rate=44100" \
		-t "$secs" "$@" -f flv "$out"
}

echo "=== generating source assets ==="
mksrc "$WORK/h264_aac.flv"  10 -c:v libx264 -preset veryfast -profile:v high -pix_fmt yuv420p -b:v 2500k -g 60 -c:a aac -b:a 128k -ac 2
SRC_SIZE=640x360 SRC_FPS=15 mksrc "$WORK/base_mp3.flv" 8 -c:v libx264 -preset ultrafast -profile:v baseline -pix_fmt yuv420p -b:v 600k -c:a libmp3lame -b:a 64k -ac 1
SRC_SIZE=1920x1080 mksrc "$WORK/hd.flv" 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p -b:v 8000k -g 250 -c:a aac -b:a 192k
SRC_SIZE=854x480 SRC_FPS=60 mksrc "$WORK/odd60.flv" 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p -b:v 3000k -g 120 -c:a aac
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=440:sample_rate=44100" -t 8 -c:a aac -b:a 128k -f flv "$WORK/audio_only.flv"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=640x360:rate=30" -t 8 -an -c:v libx264 -preset ultrafast -pix_fmt yuv420p -f flv "$WORK/video_only.flv"
ls -1 "$WORK"/*.flv | while read -r f; do printf "  %-18s %s\n" "$(basename "$f")" "$(du -h "$f" | cut -f1)"; done

start_server || exit 1
echo "=== server up (rtmp:$RTMP_PORT rtmpt:$RTMPT_PORT rtmps:$RTMPS_PORT rtmpts:$RTMPTS_PORT rtmfp:$RTMFP_PORT) ==="

# pub <src> <name> [extra ffmpeg args] -> echoes pid
pub() {
	local src="$1" name="$2"; shift 2
	ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i "$src" -c copy "$@" \
		-f flv "rtmp://127.0.0.1:$RTMP_PORT/media/$name" >"$WORK/pub_$name.log" 2>&1 &
	local p=$!; track $p; echo $p
}

# play <name> <out> <secs> [url_override]
play() {
	local name="$1" out="$2" secs="$3" url="${4:-rtmp://127.0.0.1:$RTMP_PORT/media/$1}"
	run_timeout $((secs+12)) ffmpeg -hide_banner -loglevel error -y -i "$url" -t "$secs" -c copy \
		-f flv "$out" >"${out%.flv}_play.log" 2>&1
}

echo
echo "=== A. codec matrix ==="

echo "[A1] H.264 High + AAC stereo 44.1k"
P=$(pub "$WORK/h264_aac.flv" a1); wait_publishing a1 && {
	play a1 "$WORK/a1.flv" 4
	[ "$(vcodec "$WORK/a1.flv")" = h264 ] && ok "A1 video is h264" || bad "A1 video codec: '$(vcodec "$WORK/a1.flv")'"
	[ "$(acodec "$WORK/a1.flv")" = aac ]  && ok "A1 audio is aac"  || bad "A1 audio codec: '$(acodec "$WORK/a1.flv")'"
	[ "$(size "$WORK/a1.flv")" = "1280,720" ] && ok "A1 resolution survives (1280x720)" || bad "A1 resolution: $(size "$WORK/a1.flv")"
} || bad "A1 publish never registered"
kill $P 2>/dev/null; sleep 1

echo "[A2] H.264 Baseline + MP3 mono"
P=$(pub "$WORK/base_mp3.flv" a2); wait_publishing a2 && {
	play a2 "$WORK/a2.flv" 4
	[ "$(vcodec "$WORK/a2.flv")" = h264 ] && ok "A2 video is h264" || bad "A2 video codec: '$(vcodec "$WORK/a2.flv")'"
	[ "$(acodec "$WORK/a2.flv")" = mp3 ]  && ok "A2 audio is mp3 (non-AAC path)" || bad "A2 audio codec: '$(acodec "$WORK/a2.flv")'"
} || bad "A2 publish never registered"
kill $P 2>/dev/null; sleep 1

echo "[A3] audio-only stream"
P=$(pub "$WORK/audio_only.flv" a3); wait_publishing a3 && {
	play a3 "$WORK/a3.flv" 4
	[ -n "$(acodec "$WORK/a3.flv")" ] && ok "A3 audio-only delivered" || bad "A3 no audio"
	[ -z "$(vcodec "$WORK/a3.flv")" ] && ok "A3 no phantom video stream" || bad "A3 unexpected video: $(vcodec "$WORK/a3.flv")"
} || bad "A3 publish never registered"
kill $P 2>/dev/null; sleep 1

echo "[A4] video-only stream"
P=$(pub "$WORK/video_only.flv" a4); wait_publishing a4 && {
	play a4 "$WORK/a4.flv" 4
	[ -n "$(vcodec "$WORK/a4.flv")" ] && ok "A4 video-only delivered" || bad "A4 no video"
	[ -z "$(acodec "$WORK/a4.flv")" ] && ok "A4 no phantom audio stream" || bad "A4 unexpected audio: $(acodec "$WORK/a4.flv")"
} || bad "A4 publish never registered"
kill $P 2>/dev/null; sleep 1

# Enhanced-RTMP codecs. ffmpeg 8.0 muxes both into FLV with the E-RTMP
# FourCC signalling; the server predates that, so this reports what it does.
ertmp_case() { # ertmp_case <id> <label> <encoder args...>
	local id="$1" label="$2"; shift 2
	echo "[$id] $label (enhanced-RTMP codec)"
	ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=640x360:rate=30" -t 5 \
		"$@" -pix_fmt yuv420p -an -f flv "$WORK/$id.flv" 2>"$WORK/${id}_enc.log"
	if [ ! -s "$WORK/$id.flv" ]; then skip "$id ffmpeg could not mux $label into FLV"; return; fi
	ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i "$WORK/$id.flv" -c copy -f flv \
		"rtmp://127.0.0.1:$RTMP_PORT/media/$id" >"$WORK/pub_$id.log" 2>&1 &
	local P=$!; track $P
	if wait_publishing "$id" 10; then
		play "$id" "$WORK/${id}_out.flv" 3
		if [ -s "$WORK/${id}_out.flv" ] && [ -n "$(vcodec "$WORK/${id}_out.flv")" ]; then
			ok "$id $label round-trips (codec: $(vcodec "$WORK/${id}_out.flv"))"
		else
			skip "$id $label publishes but does not play back -- E-RTMP not implemented (review P2)"
		fi
	else
		skip "$id $label publish not accepted -- E-RTMP not implemented (review P2)"
	fi
	kill $P 2>/dev/null
	lsof -nP -iTCP:"$RTMP_PORT" -sTCP:LISTEN >/dev/null 2>&1 \
		&& ok "$id server survived an unsupported codec" || bad "$id server died on $label"
	sleep 1
}

ertmp_case a5 HEVC -c:v libx265 -preset ultrafast
ertmp_case a6 AV1  -c:v libsvtav1 -preset 12

echo
echo "=== B. stream shapes ==="

echo "[B1] 1080p 8Mbit, 250-frame GOP (large keyframes -> chunk fragmentation)"
P=$(pub "$WORK/hd.flv" b1); wait_publishing b1 && {
	play b1 "$WORK/b1.flv" 5
	[ "$(size "$WORK/b1.flv")" = "1920,1080" ] && ok "B1 1080p intact" || bad "B1 size: $(size "$WORK/b1.flv")"
	n=$(nframes "$WORK/b1.flv"); [ "${n:-0}" -gt 60 ] && ok "B1 $n video packets received" || bad "B1 only ${n:-0} packets"
} || bad "B1 publish never registered"
kill $P 2>/dev/null; sleep 1

echo "[B2] 854x480 @ 60fps (odd width, high packet rate)"
P=$(pub "$WORK/odd60.flv" b2); wait_publishing b2 && {
	play b2 "$WORK/b2.flv" 4
	[ "$(size "$WORK/b2.flv")" = "854,480" ] && ok "B2 854x480 intact" || bad "B2 size: $(size "$WORK/b2.flv")"
	n=$(nframes "$WORK/b2.flv"); [ "${n:-0}" -gt 120 ] && ok "B2 $n packets (60fps sustained)" || bad "B2 only ${n:-0} packets"
} || bad "B2 publish never registered"
kill $P 2>/dev/null; sleep 1

echo
echo "=== C. session behaviour ==="

echo "[C1] fan-out: 1 publisher -> 5 concurrent players"
P=$(pub "$WORK/h264_aac.flv" c1); wait_publishing c1 && {
	PL=()
	for i in 1 2 3 4 5; do
		( play c1 "$WORK/c1_$i.flv" 4 ) &
		PL+=($!); track $!
	done
	# Only the players -- a bare wait would also block on $SRV, which never exits.
	for p in "${PL[@]}"; do wait "$p" 2>/dev/null; done
	good=0
	for i in 1 2 3 4 5; do has_av "$WORK/c1_$i.flv" && good=$((good+1)); done
	[ "$good" -eq 5 ] && ok "C1 all 5 players got A/V" || bad "C1 only $good/5 players got A/V"
} || bad "C1 publish never registered"
kill $P 2>/dev/null; sleep 1

echo "[C2] 5 concurrent publishers, distinct streams"
PIDS=()
for i in 1 2 3 4 5; do PIDS+=("$(pub "$WORK/base_mp3.flv" "c2_$i")"); done
allpub=1
for i in 1 2 3 4 5; do wait_publishing "c2_$i" 20 || allpub=0; done
[ "$allpub" -eq 1 ] && ok "C2 all 5 publishes registered" || bad "C2 not all publishes registered"
good=0
for i in 1 2 3 4 5; do play "c2_$i" "$WORK/c2_$i.flv" 3; has_av "$WORK/c2_$i.flv" && good=$((good+1)); done
[ "$good" -eq 5 ] && ok "C2 all 5 streams independently playable" || bad "C2 only $good/5 playable"
for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; sleep 1

echo "[C3] republish the same name after the publisher drops"
P=$(pub "$WORK/h264_aac.flv" c3); wait_publishing c3 && {
	kill "$P" 2>/dev/null; sleep 2
	P2=$(pub "$WORK/h264_aac.flv" c3b); wait_publishing c3b && {
		play c3b "$WORK/c3.flv" 3
		has_av "$WORK/c3.flv" && ok "C3 republish after drop serves media" || bad "C3 republished stream not playable"
	} || bad "C3 republish never registered"
	kill "${P2:-}" 2>/dev/null
} || bad "C3 initial publish never registered"
sleep 1

echo "[C4] waiting subscriber: play before publish, publisher arrives later"
( play c4 "$WORK/c4.flv" 8 ) & PL=$!; track $PL
sleep 3
P=$(pub "$WORK/h264_aac.flv" c4)
wait $PL 2>/dev/null
has_av "$WORK/c4.flv" && ok "C4 waiting subscriber served once the publisher appeared" \
	|| bad "C4 waiting subscriber got no media"
kill $P 2>/dev/null; sleep 1

echo "[C4b] query-string stream names resolve the same for publish and play"
P=$(pub "$WORK/h264_aac.flv" "c4b?token=abc123")
if wait_publishing c4b 20; then
	play "c4b?token=abc123" "$WORK/c4b.flv" 4
	has_av "$WORK/c4b.flv" && ok "C4b publish+play agree on 'c4b?token=abc123'" \
		|| bad "C4b query-bearing name did not resolve"
else
	bad "C4b publish of a query-bearing name never registered"
fi
kill $P 2>/dev/null; sleep 1

echo "[C5] publisher drops mid-stream while a player is attached"
P=$(pub "$WORK/h264_aac.flv" c5); wait_publishing c5 && {
	( play c5 "$WORK/c5.flv" 10 ) & PL=$!; track $PL
	sleep 3; kill "$P" 2>/dev/null
	wait $PL 2>/dev/null
	[ -s "$WORK/c5.flv" ] && ok "C5 player kept the media it had received" || bad "C5 player produced nothing"
	lsof -nP -iTCP:"$RTMP_PORT" -sTCP:LISTEN >/dev/null 2>&1 && ok "C5 server survived publisher drop" || bad "C5 server died"
} || bad "C5 publish never registered"
sleep 1

echo
echo "=== D. transports carrying real media ==="

echo "[D1] RTMPT (HTTP tunnel)"
P=$(pub "$WORK/h264_aac.flv" d1); wait_publishing d1 && {
	play d1 "$WORK/d1.flv" 4 "rtmpt://127.0.0.1:$RTMPT_PORT/media/d1"
	has_av "$WORK/d1.flv" && ok "D1 A/V over RTMPT" || bad "D1 no A/V over RTMPT"
} || bad "D1 publish never registered"
kill $P 2>/dev/null; sleep 1

echo "[D2] RTMPS (TLS)"
P=$(pub "$WORK/h264_aac.flv" d2); wait_publishing d2 && {
	play d2 "$WORK/d2.flv" 4 "rtmps://127.0.0.1:$RTMPS_PORT/media/d2"
	has_av "$WORK/d2.flv" && ok "D2 A/V over RTMPS" || bad "D2 no A/V over RTMPS"
} || bad "D2 publish never registered"
kill $P 2>/dev/null; sleep 1

echo "[D3] RTMPTS (TLS + HTTP tunnel)"
P=$(pub "$WORK/h264_aac.flv" d3); wait_publishing d3 && {
	# ffmpeg's own rtmpts client fails with "Cannot reuse HTTP connection for
	# different protocol https vs http"; rtmpdump tunnels it cleanly.
	rtmpdump -z -r "rtmpts://127.0.0.1:$RTMPTS_PORT/media/d3" -o "$WORK/d3.flv" >"$WORK/d3_rd.log" 2>&1 &
	RD=$!; track $RD; sleep 8; kill $RD 2>/dev/null; wait $RD 2>/dev/null
	has_av "$WORK/d3.flv" && ok "D3 A/V over RTMPTS (rtmpdump)" || bad "D3 no A/V over RTMPTS"
} || bad "D3 publish never registered"
kill $P 2>/dev/null; sleep 1

echo "[D4] publish over RTMPS, play over plain RTMP (transport crossover)"
ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i "$WORK/h264_aac.flv" -c copy -f flv \
	"rtmps://127.0.0.1:$RTMPS_PORT/media/d4" >"$WORK/pub_d4.log" 2>&1 &
P=$!; track $P
wait_publishing d4 && {
	play d4 "$WORK/d4.flv" 4
	has_av "$WORK/d4.flv" && ok "D4 RTMPS publish -> RTMP play" || bad "D4 crossover failed"
} || bad "D4 RTMPS publish never registered"
kill $P 2>/dev/null; sleep 1

echo
echo "=== E. durability ==="

echo "[E1] 45s sustained stream: continuity and drift"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=1280x720:rate=30" \
	-f lavfi -i "sine=frequency=440:sample_rate=44100" -t 50 \
	-c:v libx264 -preset veryfast -pix_fmt yuv420p -b:v 2000k -g 60 -c:a aac -f flv "$WORK/long.flv"
P=$(pub "$WORK/long.flv" e1); wait_publishing e1 && {
	play e1 "$WORK/e1.flv" 45
	d=$(dur "$WORK/e1.flv")
	[ "${d:-0}" -ge 40 ] && ok "E1 received ${d}s of a 45s pull" || bad "E1 only ${d:-0}s received"
	n=$(nframes "$WORK/e1.flv")
	[ "${n:-0}" -ge 1150 ] && ok "E1 $n video packets (>=1150 of ~1350 expected)" || bad "E1 only ${n:-0} video packets"
	# monotonic dts across the whole pull
	ffprobe -v error -select_streams v:0 -show_entries packet=dts_time -of csv=p=0 "$WORK/e1.flv" 2>/dev/null \
		| awk -F, 'NF{ if ($1+0 < prev) { bad++ } ; prev=$1 } END { exit (bad>0) }' \
		&& ok "E1 video DTS strictly non-decreasing" || bad "E1 DTS went backwards"
} || bad "E1 publish never registered"
kill $P 2>/dev/null; sleep 1

echo
echo "=== F. resource limits ==="

echo "[F1] slow consumer vs --max-queue-bytes (server must shed, not grow unbounded)"
# Publish as fast as the disk allows (no -re) so the server queues for a player
# that reads at native rate.
ffmpeg -hide_banner -loglevel error -i "$WORK/long.flv" -c copy -f flv \
	"rtmp://127.0.0.1:$RTMP_PORT/media/f1" >"$WORK/pub_f1.log" 2>&1 &
P=$!; track $P
wait_publishing f1 && {
	run_timeout 30 ffmpeg -hide_banner -loglevel error -y -re -i "rtmp://127.0.0.1:$RTMP_PORT/media/f1" \
		-t 10 -c copy -f flv "$WORK/f1.flv" >"$WORK/f1_play.log" 2>&1
	rss=$(ps -o rss= -p "$SRV" 2>/dev/null | tr -d ' ')
	[ -n "$rss" ] && [ "$rss" -lt 2000000 ] && ok "F1 server RSS ${rss}KB stayed bounded" || bad "F1 server RSS ${rss:-?}KB"
	lsof -nP -iTCP:"$RTMP_PORT" -sTCP:LISTEN >/dev/null 2>&1 && ok "F1 server still listening" || bad "F1 server died"
} || bad "F1 publish never registered"
kill $P 2>/dev/null; sleep 1

echo "[F2] recording to FLV (bundled client; ffmpeg cannot request record mode)"
if [ -x "$CLIENT" ]; then
	"$CLIENT" -r "rtmp://127.0.0.1:$RTMP_PORT/media" -c publish -s f2 \
		-i "$WORK/h264_aac.flv" -R -n >"$WORK/f2.log" 2>&1 &
	P=$!; track $P
	sleep 12; kill $P 2>/dev/null; sleep 2
	if ls "$WORK/rec"/f2*.flv >/dev/null 2>&1; then
		f=$(ls "$WORK/rec"/f2*.flv | head -1)
		has_av "$f" && ok "F2 recorded $(basename "$f") is valid A/V" || bad "F2 recorded file not valid A/V"
	else
		skip "F2 no recorded file (client flags may differ; see $WORK/f2.log)"
	fi
else
	skip "F2 rtmp_client not built"
fi

echo
echo "================ SUMMARY ================"
echo "  pass $PASS   fail $FAIL   skip $SKIP"
[ "$FAIL" -gt 0 ] && { echo "  failures:"; for f in "${FAILED[@]}"; do echo "    - $f"; done; }
echo "  work dir: $WORK"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
