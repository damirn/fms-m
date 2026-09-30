#!/usr/bin/env bash
# Drive a sanitizer-built server with real clients and fail on any report.
#
# The unit suite runs under sanitizers already, but the server's own threads --
# the io_context pool, the stats timer, the RTMFP reaper -- only exist in a
# running server, so the races and lifetime bugs that live there need a workload.
#
# Build first, e.g.
#   cmake -S . -B build-tsan  -DSANITIZE=thread  -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake -S . -B build-asan  -DSANITIZE=address -DCMAKE_BUILD_TYPE=RelWithDebInfo
# then: test/interop/sanitize.sh build-tsan/fms-m
#
# LeakSanitizer is unavailable on Apple platforms, so leak checking needs a Linux
# build (the Dockerfile takes --build-arg SANITIZE=address).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FMS="${1:-$ROOT/build-tsan/fms-m}"
CLIENT="${CLIENT:-$(dirname "$FMS")/rtmp_client}"
RTMFP_CPP="${RTMFP_CPP:-$ROOT/../rtmfp-cpp/test}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/fms-sanitize.XXXXXX")"
RTMP_PORT=28600; RTMPT_PORT=28601; RTMFP_PORT=28602
DURATION="${DURATION:-18}"

[ -x "$FMS" ] || { echo "no sanitizer build at $FMS"; exit 2; }

# A non-sanitized binary produces no reports and would exit green.
if ! nm "$FMS" 2>/dev/null | grep -qE '__(tsan|asan|ubsan)_'; then
	echo "$FMS has no sanitizer runtime linked; build with -DSANITIZE=" >&2
	exit 2
fi

# Both workloads are conditional below; without either, a green run means nothing.
[ -x "$CLIENT" ] || [ -x "$RTMFP_CPP/tcpublish" ] || {
	echo "neither $CLIENT nor $RTMFP_CPP/tcpublish is present; nothing would run" >&2
	exit 2
}

mkdir -p "$WORK/rec" "$WORK/logs"

command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg required"; exit 2; }
ffmpeg -loglevel quiet -f lavfi -i "testsrc=d=$((DURATION + 10)):s=320x240" \
	-f lavfi -i "sine=d=$((DURATION + 10))" \
	-c:v libx264 -preset ultrafast -c:a aac -y "$WORK/src.flv" 2>/dev/null

# Reports go to files so a crashed server still leaves its findings behind. One
# prefix for all three: the runtime parses every *SAN_OPTIONS it is given and the
# last log_path wins, so separate paths would silently land in one file anyway.
export TSAN_OPTIONS="halt_on_error=0:log_path=$WORK/san"
# detect_leaks is unsupported on Apple and is the whole point on Linux.
if [ "$(uname -s)" = "Darwin" ]; then DETECT_LEAKS=0; else DETECT_LEAKS=1; fi
export ASAN_OPTIONS="halt_on_error=0:detect_leaks=$DETECT_LEAKS:log_path=$WORK/san"
export UBSAN_OPTIONS="print_stacktrace=1:log_path=$WORK/san"

for p in "$RTMP_PORT" "$RTMPT_PORT"; do
	nc -z 127.0.0.1 "$p" 2>/dev/null && { echo "port $p already bound; a stale server would be driven instead" >&2; exit 2; }
done

"$FMS" -R "$RTMP_PORT" -T "$RTMPT_PORT" -K "$RTMFP_PORT" -t 4 \
	-o "$WORK/rec" -P "$WORK/logs" >"$WORK/server.out" 2>&1 &
SRV=$!
for _ in $(seq 1 80); do
	nc -z 127.0.0.1 "$RTMP_PORT" 2>/dev/null && break
	sleep 0.5
done
nc -z 127.0.0.1 "$RTMP_PORT" 2>/dev/null || { echo "server did not start; see $WORK/server.out"; exit 2; }

echo "=== workload (${DURATION}s, 4 io threads) ==="
if [ -x "$CLIENT" ]; then
	"$CLIENT" -r "rtmp://127.0.0.1:$RTMP_PORT/media" -c publish -s s1 -i "$WORK/src.flv" -R -n >/dev/null 2>&1 &
	sleep 2
	# Fan-out: the send-queue and stats paths run on several io threads at once.
	for _ in 1 2 3; do
		"$CLIENT" -r "rtmp://127.0.0.1:$RTMP_PORT/media" -c play -s s1 -n >/dev/null 2>&1 &
	done
	echo "  rtmp: 1 publisher (recording) + 3 subscribers"
fi
if [ -x "$RTMFP_CPP/tcpublish" ] && [ -x "$RTMFP_CPP/tcconn" ]; then
	"$RTMFP_CPP/tcpublish" -4 "rtmfp://127.0.0.1:$RTMFP_PORT/media#r1" "$WORK/src.flv" >/dev/null 2>&1 &
	sleep 2
	"$RTMFP_CPP/tcconn" -4 "rtmfp://127.0.0.1:$RTMFP_PORT/media#r1" >/dev/null 2>&1 &
	echo "  rtmfp: publish + play (fragment reassembly, session reaper)"
fi
# Connections abandoned mid-handshake, so the handshake timer path is exercised.
for _ in $(seq 1 15); do printf '\x03' | nc -w 1 127.0.0.1 "$RTMP_PORT" >/dev/null 2>&1 & done
echo "  15 connections abandoned mid-handshake"

sleep "$DURATION"
kill -0 "$SRV" 2>/dev/null || { echo "server exited during the workload; see $WORK/server.out" >&2; cat "$WORK"/san.* 2>/dev/null; exit 1; }
[ -n "${CLIENT:-}" ] && pkill -f "$(basename "$CLIENT")" 2>/dev/null
pkill -f "$RTMFP_CPP/tc" 2>/dev/null
sleep 2
# Long enough for the at-exit leak check and the report flush before SIGKILL.
kill -INT "$SRV" 2>/dev/null; sleep 10; kill -9 "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null; srv_status=$?

echo
# Attribute by signature, not by file name: all three write to the same prefix.
reports="$(cat "$WORK"/san.* 2>/dev/null)"
total=0
# LSan heads its report with "ERROR: LeakSanitizer" and summarises as
# "SUMMARY: AddressSanitizer: ... leaked", so the ASan signature matches neither.
for spec in "tsan:WARNING: ThreadSanitizer" "asan:ERROR: AddressSanitizer" \
            "lsan:ERROR: LeakSanitizer" "ubsan:runtime error:"; do
	kind="${spec%%:*}"; sig="${spec#*:}"
	n=$(printf '%s' "$reports" | grep -cF "$sig")
	n=${n:-0}
	[ "$n" -gt 0 ] && echo "  $kind: $n report(s)"
	total=$((total + n))
done
if [ "$total" -eq 0 ]; then
	# A sanitizer can also report only through the exit status (LSan's exitcode,
	# ASan's default 1), which the wait above would otherwise swallow.
	case "$srv_status" in
		0|130|137|143) echo "  no sanitizer reports"; echo "  work dir: $WORK"; exit 0 ;;
		*) echo "  server exited $srv_status with no report text; see $WORK/server.out"
		   tail -30 "$WORK/server.out" 2>/dev/null
		   echo "  work dir: $WORK"
		   exit 1 ;;
	esac
fi
echo
printf '%s\n' "$reports" | head -60
echo "  work dir: $WORK"
exit 1
