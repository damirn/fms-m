# Slow consumers and outbound backpressure

What the server does when a subscriber stops reading — what Adobe FMS 4.5 does
(measured, not inferred), what we do, and why they differ.

## The problem

A subscriber that stops reading applies TCP backpressure. Its in-flight write
never completes, so `rtmp_connection::handle_notify` never drains, and every frame
the publisher produces keeps landing in that connection's
`rtmp_application::m_async_messages` queue. Before this was bounded, the queue had
no cap and no drop policy: a single wedged client grew the server's RSS with the
live stream until the process died.

Measured on this codebase (1280x720, 6 Mbit/s publish, one stalled subscriber, 50 s):
RSS **12.5 MB → 54.2 MB**, still climbing linearly, client never dropped. The
server had buffered ~48 MB for one client and delivered all of it once the client
resumed. Reproduce with `--max-queue-bytes 0`, which restores that behaviour.

Four config options looked like they addressed this — `max-audio-frames`,
`max-audio-frames-high-latency`, `notify-threshold`, `terminate-threshold`. They
were parsed, documented in `--help`, and **read by no code path at all**. They
were also on the wrong axis (message counts and milliseconds; see below). They
have been removed rather than wired up.

## What FMS 4.5 actually does

Measured against a stock Flash Media Server 4.5.0 r297 in a container
(`test/interop/fms-ref/`), publishing 1280x720 and playing with a `rtmpdump` whose
stdout was blocked, with a healthy subscriber alongside as a control:

| publish rate | subscriber | FMS `x-duration` | `sc-bytes` | `x-status` | backlog at kill |
|---|---|---|---|---|---|
| ~813 kB/s | stalled (reads nothing) | 12 s | 0.74 MB | **416** | ~9.0 MB |
| ~813 kB/s | reads 200 kB/s | 15 s | 4.63 MB | **416** | ~7.6 MB |
| ~240 kB/s | stalled | 49 s | 0.39 MB | **416** | ~11.4 MB |
| — | reads normally (×3) | 120 / 130 / 140 s | up to 97 MB | 200 | — |

1. **FMS bounds the backlog and disconnects.** It does not queue without limit.
2. **The bound is in BYTES (~8–10 MB), not time.** That is what the third row is
   for: quartering the bitrate *quadrupled* survival (12 s → 49 s) while the byte
   backlog stayed flat. A media-time bound would have killed it at ~12 s again.
   This is why the removed count/millisecond options were the wrong shape.
3. **It does not thin the stream to keep a slow client alive.** The 200 kB/s
   reader got no `InsufficientBW`, no extra `BufferEmpty` — killed at the same
   byte threshold as the fully wedged one.
4. **Nothing is sent at RTMP level before the drop.** No `onStatus`, no
   user-control. The subscriber's last messages are the ordinary start sequence
   (`Play.Reset` → `Play.Start` → `BufferEmpty(31)` → `BufferReady(32)`).
   The client only ever learns via TCP.
5. **The disconnect is a hard RST, and it is *deferred*.** With the client's
   receive window at zero, FMS cannot deliver a FIN; the RST only materialises
   when the client next touches the socket. In run 1 that was 26 µs after the
   client woke up — a full 60 s after FMS had actually dropped it. **You cannot
   detect this from the client side by timing**; the access log is the only
   ground truth.
6. FMS records it distinguishably: access-log `x-status=416` on the `disconnect`
   record for every slow client, `200` for every healthy one.

FMS's own shipped `Application.xml` documents `Client/MsgQueue/Live/MaxAudioLatency`
(2000 ms, "Drop live audio if audio q exceeds time specified") and
`MinBufferTime` (8000 ms), but no ~9 MB knob is exposed anywhere in its config —
the hard cap is internal. Note also that `MaxWriteDelay`/`MinWriteDelay` (20 s/12 s)
sit under `Adaptor/HTTPTunnel` and so govern **RTMPT only**, not plain RTMP.

## What we do

We keep FMS's byte-bounded shape and its hard cap, but shed before we kill.
`--max-queue-bytes` (default **10 MB**, the middle of FMS's measured band; `0`
disables the bound entirely) drives three tiers:

| queued bytes | behaviour |
|---|---|
| ≤ cap/2 | normal — nothing happens |
| > cap/2 | shed droppable video down to cap/4, then keep streaming |
| > cap after shedding | nothing left to give — disconnect (what FMS does) |

Shedding order is by how expensive the loss is (`send_queue_policy.h`):
inter/disposable frames first, then whole GOPs (keyframes), oldest first — the
client is behind, so the freshest content is the useful content. **Audio, codec
sequence headers (AVC/AAC config) and all control/command messages are never
shed**: audio is ~2% of the bytes but far more noticeable when it gaps, and losing
a sequence header makes the stream undecodable for the rest of the session.

The hysteresis (shed to cap/4, re-arm at cap/2) means a saturated queue is thinned
in bursts rather than dropping one frame per enqueue forever after.

This is strictly gentler than FMS and **invisible on the wire** — no new message
types, no protocol divergence. A client cannot tell the difference except by
surviving congestion FMS would have killed it for.

### Measured result

Same harness, same stream, one stalled subscriber, healthy control alongside:

| | RSS growth | subject received | video frames | audio frames | outcome |
|---|---|---|---|---|---|
| `--max-queue-bytes 0` (old) | **+40 MB** | 48.8 MB | 1600 | 2732 | never dropped |
| default 10 MB, stalled | **+11 MB** | 14.4 MB | 423 | 2732 | survived, thinned |
| default 10 MB, reads 200 kB/s | **+14 MB** | 15.2 MB | 470 | 2499 | survived, thinned |

The healthy control received 50.4 MB in every run — unaffected. Audio is
essentially untouched while video is thinned ~75%, and the thinned output still
decodes end-to-end as h264+aac for the full duration.

Shed and disconnect decisions are logged (rate-limited per connection), and shed
messages are counted into the per-netstream `m_messages_dropped` stat, which the
admin app reports and which was previously always zero.

## Reproducing

FMS oracle (needs the proprietary tarball, see `test/interop/fms-ref/README.md`):

```sh
docker build -t fms45 test/interop/fms-ref && docker run -d --name fms45 -p 11935:1935 fms45
# stall a stock client by blocking its stdout; read FMS's access log for the verdict
rtmpdump -z -v -r rtmp://localhost:11935/live/x -o - | (sleep 60; cat > /dev/null)
docker exec fms45 grep disconnect /opt/fms/logs/access.00.log   # x-status 416 == dropped as slow
```

Against fms-m, `--max-queue-bytes 0` reproduces the unbounded behaviour for an
A/B. Unit coverage for the classification and the two-pass shed is
`test/send_queue_policy_test.cpp`.
