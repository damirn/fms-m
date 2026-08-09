# fms-m — feature gaps & backlog

Gap analysis vs reference implementations: **rtmpdump/librtmp** (RTMP client lib),
**zenomt/rtmfp-cpp** and **MonaSolutions/librtmfp** (RTMFP clients). "Gap" = a
protocol capability those exercise that our *server* can't serve or interop with.
File refs are indicative anchors, not exhaustive.

**Top-level goal:** 100% interop with rtmpdump + rtmfp-cpp, verified by an automated
interop matrix (see the *Interop test matrix* item). Legend: **[P1]**
interop-breaking, or a correctness/resource-safety defect · **[P2]** meaningful
capability · **[P3]** nice-to-have / hardening.

Priority-sorted. Subsystem tag in parentheses. Updated 2026-09-03.

> **Recently landed** entries used to accumulate here with commit SHAs and dated
> round headings -- 72 lines of it. That is `git log`, and this file is a
> forward-looking gap analysis; the history was removed on 2026-09-03. For what
> has shipped, read the log. For open review findings, see `REVIEW_2026-09.md`.

---

## P1 — correctness / resource safety

*(The transport P1 is empty — RTMPS and RTMPTS both landed. What remains here is
correctness and resource safety.)*

- **`std::atomic_load`/`atomic_store` on `shared_ptr` are REMOVED in C++26.** Used
  for the avc/aac config slots on the fan-out path. Deprecated in C++20, and we
  already build at `-std=c++23`, so this is a dated build break rather than a
  style point. Swap to `std::atomic<std::shared_ptr<T>>`.
  (`av_delivery.cpp:32,139,140,242,349`, `stream_registry.h:70-75`)
- **Origin-pull helper spawning is unbounded and runs under the media lock.**
  `spawn_helper` is called from `handle_invoke_play` while the registry's EXCLUSIVE
  lock is held, so a `fork()` (page-table copy of the whole server) stalls every
  live subscriber's fan-out — the same defect the VOD reader was just moved off.
  It is also called once per remote-stream play with no dedup, rate limit or cap,
  so repeated plays fork repeatedly; and the origin URL is fully client-supplied,
  making the server issue outbound connections to arbitrary hosts. All three are
  gated on `--helper-app` being configured (off by default), which is the only
  reason this is not urgent. (`remote_relay.cpp:29-79`, `media_application.cpp:386`)

## P2 — meaningful capability

### RTMP (librtmp / rtmpdump)
- **Enhanced RTMP (E-RTMP) FourCC codecs.** librtmp + rtmfp-cpp speak the codec
  extension: video HEVC/AV1/VP9/VP8/VVC, audio Opus/AC3/E-AC3/FLAC, the `_EX`
  messages (COMMAND_EX 0x11, DATA_EX 0x0f, SHAREDOBJ_EX 0x10), and multitrack. Our
  config-frame caching assumes AVC/AAC layout, so modern OBS HEVC/AV1 would
  mis-cache. Pass-through parity at minimum.
- **`BufferEmpty`(31)/`BufferReady`(32) for live subscribers — landed** (`ce436cb`),
  meaningful-transitions model, verified against a stock **FMS 4.5** container: emit
  BufferEmpty right after Play.Start (a live buffer starts empty) and BufferReady when
  the first frame flows. Matches both FMS start sequences (publisher-live: 31→32;
  subscriber-waiting: 31 alone then 32 on publish). Interop case 1 asserts both.
  *Deliberately not reproduced:* FMS's continuous per-gap 31/32 chatter at the live
  edge (advisory only, not needed for interop). `StreamDry`(2) still unemitted.
- **Live unpublish signal — fixed** (`11945e5`): fms-m now sends the subscriber
  BufferEmpty(31) drain + `Play.UnpublishNotify` (was StreamEOF(1), a VOD-only
  signal), matching FMS 4.5. rtmpdump + ffmpeg both close cleanly (verified against
  fms-m and stock FMS); interop case 7 guards it. *Still open (persistent clients
  only):* FMS keeps the subscriber connected to **resume on republish** (re-park as a
  waiting client + re-arm the buffer-empty flag so a republish re-emits 32). fms-m
  still tears the subscriber down on unpublish; rtmpdump/ffmpeg close anyway so they're
  unaffected. (`stream_registry::remove_broadcaster`, the waiting-client promote path)
- **SWF verification enforcement (SecureToken).** Support *requiring* SWFVerification
  so `rtmpdump --swfVfy` interops and anti-leech works.
- **play2 / playlist + reset semantics.** librtmp's play2/switch and
  `NetStream.Play.Reset` transitions; rtmpdump playlist / `--start` resume.

### RTMFP (rtmfp-cpp / librtmfp)
- **DH group 14 (2048-bit).** Implement group 14 so we don't depend on clients
  negotiating down to group 2; full FlashCrypto parity. `evp_dh` is group-agnostic
  but only ever called with the group-2 prime. (`dh2.cpp`, `service.cpp`)
- **Robust flow / congestion control.** Real advertised receive window (not hardcoded
  `0x7f`), `m_outstanding_bytes` tracking, correct retransmit under loss. *Done when:*
  a multi-MB `tcpublish→tcconn` transfer completes intact under simulated loss.
  (`session.cpp`)
- **NetGroup / P2P parity.** Full group semantics + peer introduction so rtmfp-cpp P2P
  (`tcconn` group mode) interops, beyond today's join + peer-list skeleton. No group
  media relay yet; P2P is introducer-only. (`group.h`, `session.cpp`, `redirect_ihello`)
- **Chunk types we don't parse** (RFC 7016): **FRAGMENT (0x7f)** packet-level
  fragmentation for over-MTU packets (long control/data packets break without it);
  **ACK_BITMAP (0x50)**; **BUFFERPROBE (0x18)**; **CLOSE request (0x0c)** (we only
  handle CLOSE_ACK 0x4c, so can't cleanly initiate/respond to a close);
  **ECN_REPORT (0xec)**; **RHELLO_COOKIE_CHANGE (0x79)**.
- **librtmfp app-resolution** (`NetConnection.Connect.InvalidApp`) — **root-caused +
  fixed server-side** (`7b4ed4b`): `match_app` now strips a leading `/` and a trailing
  `?query` from the connect `app`, so the raw RFC-3986 path `/media` librtmfp sends
  resolves instead of matching an empty name. Proven via `rtmpdump -a` (interop case
  6). *Still needs a real librtmfp run to close* — not available locally; the RTMP `-a`
  proxy stands in for now. Note: librtmfp's "Bad RTMFP CRC" was root-caused as *their*
  OpenSSL-3 padding bug, not ours — no fms-m change needed.

### Media / server
- **Shared-object scope + persistence.** Reclaiming an object when its last client
  goes (fixed in 2.0.0) matches FMS's NON-persistent objects. Two FMS behaviours we
  still do not have, both optional: a client may request **persistence**
  (`SharedObject.getRemote(name, uri, true)`), committing the object to `StorageDir`
  (`AutoCommit` on by default) so it survives instance unload and server restart;
  and FMS **bounds** them -- `MaxSharedObjects` 50000 per vhost, plus
  `MaxProperties`/`MaxPropertySize` per object. We cap nothing. On FMS the app
  instance is the unit of both isolation and lifetime for these (it goes idle after
  its last client, then unloads, with `AppInstanceGC` sweeping SharedObjects) --
  which ties to the item below.
- **App instances are parsed but not honoured.** `connect_router::match_app`
  already splits `media/roomA` into app + instance and stores it on the session
  (`client_session::m_app_instance`), and it is threaded through
  `delete_connection(conn_id, app_instance)` — but the base implementation does not
  even name that parameter, and nothing on the media path scopes by it:
  `add_broadcaster`/`broadcaster_for_name` use the bare stream name, and
  `so_manager` is instance-agnostic. So `rtmp://host/media/roomA/x` and
  `rtmp://host/media/roomB/x` are the same stream here and are different streams on
  FMS, where an instance is the unit of isolation *and* of lifetime (it goes idle
  after its last client, then unloads — see the shared-object note in P1).
  The sole exception is `video_call_application`, which does scope by instance
  (`m_instance_to_client`, one mixer and client set per room) — so the plumbing is
  proven, it just is not applied to streams or shared objects. Either honour the
  instance in the stream/SO namespaces or stop accepting it silently.
  (`connect_router.cpp:31-36`, `rtmp_application.cpp:105`, `media_application.cpp:290,374`)
- **HLS / DASH / fMP4 output.** FLV recording is the only output container.
- **Native relay** (push-to-remote / pull-from-origin). "Pull" is an external
  `execvp`'d helper today; no edge/origin clustering.

### Infra
- **Interop test matrix — extend the existing harness.** `test/interop/interop.sh`
  now covers RTMP live/VOD + RTMPT (rtmpdump/ffmpeg) and RTMFP live + RTMFP→RTMP
  bridge (rtmfp-cpp, strict crypto — the live case is already a two-client RTMFP
  publish→play relay), plus **RTMPS/RTMPTS** (cases 8/8b, cert minted on the fly).
  Still open: **RTMPE/RTMPTE** (blocked on the rtmpdump ARM bus-error — try a Linux
  runner or librtmfp), an **`fcclient` raw-handshake** case, a **loss/congestion**
  RTMFP transfer (ties to the flow-control P2 item), and a **slow-consumer** case
  (the shed/drop policy is covered by unit tests + a manual harness, not the matrix).

## P3 — nice-to-have / hardening

### Testing (found during the P4/P5 pass — do before touching those paths)
- **RTMFP `session`/`service` have no test at any level.** The whole lock-free
  orchestration is unverified except by manual rtmfp-cpp interop on the Linux box.
  This is the highest-leverage test to add: it's what makes the deferred RTMFP
  session-path fixes below unsafe to attempt today. (See `docs/concurrency.md`.)

### Structural design debt (second design review, 2026-08-05)

Priority order within this block; the first item is the one the others hang off.

1. **`rtmp_application` is a god base class** (~48 declarations). Every app inherits
   bandwidth-check, shared objects, result handlers, the async queue, the delay map
   and stats whether it uses them or not — `admin_application` needs none of the
   media machinery. Composition, not inheritance; the "invoke string-ladder" item
   below is a symptom of the same thing.
2. **RTMFP is the risk concentration**: a second full protocol stack with its own
   lock-free model, reaching into `rtmp_app_manager` directly, and no tests at any
   level. Ties to the testing item above and the RTMFP seam item below.

### Structural design debt (whole-project design review, 2026-07-30)
- **Latent ownership defects (small, bounded fixes):**
  - ~~`shared_object::event` double-free on copy~~ — **done** (2026-09, L1): now a
    `std::vector`, so the destructor and the rule-of-3 hole are both gone.
  - ~~`amf0_util::get_ref<std::uint32_t>` aliasing UB~~ — **done** (2026-09, D3):
    the whole family was deleted, as it had no call sites.
  - `flv_reader::read_uint32_3`/`read_uint32` accumulate a byte that `istream::read`
    leaves untouched on a short read, so a truncated FLV composes the length from an
    indeterminate value. Callers currently discard it (they test the stream after),
    so it is UB without a known consequence — initialise `b` and check the read.
    (`flv_reader.cpp:107-133`)
  - RTMFP chunk memory is raw `new`/`delete` with an unwritten "view vs. copy" rule —
    some chunks hold `const uint8_t*` into the packet buffer that is freed when
    `parse()` returns. The same dangling-view bug has been patched repeatedly. Parser
    should hand `unique_ptr<chunk>`; any chunk that outlives the packet owns its buffer.
    (`rtmfp/chunk.h`, `rtmfp/parser.cpp`)
  - RTMFP control replies share one `m_ready_chunk` raw slot, hand-freed in six sites
    (already leaked once). Replace with an owning `deque<unique_ptr<chunk>>` drained in
    the send pass — also lifts the "one control reply per packet" limitation.
    (`rtmfp/session.cpp`) — the *redirect* queue next to it became
    `queue<pair<endpoint, unique_ptr<chunk>>>` in 2026-09 (M30); this slot did not.
- **Layer the codec / data model / framing (RTMP).** The `amf0` codec (+ its per-decode
  reference table) is a member of *every* `rtmp_message`, so a ChunkSize/audio/video
  message drags AMF decode state; `rtmp_protocol::deserialize_*` is 15 boilerplate
  helpers shadowing the message `deserialize` virtuals; `byte_reader` exposes two
  inconsistent APIs (a byte-order-aware non-throwing family + a naive throwing one that
  forces ~20 scattered manual `ntoh` swaps); `byte_writer` fuses a serializer and an
  async-fill accumulator under debug-only asserts. Make the codec a free service over
  byte ranges (leaves `rtmp_message` a pure data object + `rtmp_protocol` a thin
  framing/factory layer), give the throwing reader the same typed BE/LE reads, split
  `byte_writer`. (`rtmp_message.h:102`, `rtmp_protocol.cpp:15-61`, `byte_reader.h`, `byte_writer.h`)
- **Replace the invoke string-ladders with per-app dispatch tables.** Every app plus
  the base re-implements an `if (fn == …)` chain, so a new verb edits a chain and every
  subclass carries base machinery (bw-check, SO) it may not use. Register a
  `unordered_map<string_view, handler>` per app in its ctor (base pre-fills the common
  verbs). (`rtmp_application.cpp:220-241`, each app's `handle_invoke`)
- **Give the RTMFP transport a testable seam (prereq for the Testing item above).**
  `session` is both the RFC-7016 transport engine and the RTMP demux/dispatch (it
  includes `rtmp_protocol.h`/`rtmp_app_manager.h`), constructed only against live
  sockets/timers — which is *why* the tier has no tests. Introduce a narrow
  `flow_message_sink(stream_id, bytes)` boundary and split I/O binding (`open()`/`start()`)
  out of the constructors; inject the app-manager + a clock. (`rtmfp/session.cpp`, `rtmfp/service.cpp`)
- **Finish the `get_connection` → `get_connection_opt` migration.** The throwing
  overload is still used on live paths (e.g. the connect route). (`connect_router.cpp`,
  `connection_registry.cpp`)
- **Make `client/` an actual library.** The shipped `nc`/`ns_event_handler` are
  hard-wired to the CLI `config` singleton, so both real consumers (helper tool, b2b
  test) bypass them and reimplement handlers; sink ownership is raw-pointer with leak
  paths; `net_connection` is a 712-line god object. Split the CLI (`main`, handlers,
  `config`) from the reusable core, and export a typed status-code vocabulary (the
  `NetConnection.Connect.*` / `NetStream.*` strings are re-typed in ≥4 places today).
- **Inject `config` instead of reaching the singleton.** `config::instance()` is
  pulled from ~14 files across every layer (server, rtmpt_manager, remote_relay, the
  apps, codecs, the whole client) — a global that blocks unit-testing those classes in
  isolation. Pass the needed config in rather than reading the singleton in ctors.

### RTMP / RTMPT correctness (deferred — real clients don't hit these)
- **RTMP simple-handshake fallback** for a versioned C1 with no valid digest (all
  real clients sign).
- **RTMPT out-of-order stash drain** on an `/idle` gap-filler (needs multi-connection
  reordering); **partial handshake split across POSTs** (real clients send it whole).
- **librtmp connect-param coverage** (`auth`/`token`/`subscribe`/`tcUrl`/`swfUrl`/…)
  and command verbs we don't handle: `secureTokenResponse`, `set_playlist`/
  `playlist_ready`, client-initiated `_checkbw`, `onFCSubscribe`/`onFCUnsubscribe`.

### RTMFP correctness (deferred — needs the test harness above first)
- **`handle_flow_exception_report` is a documented no-op.** Peer-initiated sending-flow
  teardown needs coordinated removal across `m_sending_flows` +
  `m_flow_id_to_stream_id`/`m_stream_id_to_flow_id` + the owning app (the flow-
  lifecycle work). (`session.cpp`)
- **One control-reply per received packet.** `m_ready_chunk` is a single slot cleared
  each packet; sending multiple control replies (e.g. close-ack *and* ping-reply) in
  one packet would need a send-scheduling redesign. Leak on that path is now fixed.
- **Unverified redirect / glare path** in the P2P introducer. (`redirect_ihello`)
- **Partial-reliability USERDATA semantics** — ABN (abandon) flag + MANDATORY_CUTOFF
  option, per-flow priority scheduling (PRI_*).
- **No FEC.**

### Performance
- **Zero-copy send (scatter-gather).** The last per-subscriber copy is
  `serialize`→`chunk_buffer` into the output buffer. Removing it needs
  scatter-gather `writev` of [owned header][shared-payload view]. BLOCKER: RTMPE
  encrypts output in place per connection, so the payload can't be shared for
  encrypted streams — helps plaintext RTMP only, and is a big write-path rewrite.
  Scope carefully or drop.
- **AMF is allocation-heavy.** Every value is `shared_ptr<amf0_x>(new …)`; a
  value/variant design would be lighter on the command hot path. Taste/perf, not
  correctness — the AMF layer is otherwise the strongest, best-tested part.

### Infra / ops
- **Graceful drain on shutdown.** SIGINT/SIGTERM `stop()`s, abandoning queued frames.
  A real drain must run handlers to completion — unsafe today because teardown relies
  on handlers being abandoned (see `docs/concurrency.md` and the note at
  `server::stop()`); needs the acceptor + app/manager timers cancelled first.
- **CI** — unit + fuzz + b2b + bench exist, nothing runs them.
- **Prometheus-style metrics endpoint** (admin stats exist over RTMP).
- **Wire the auth framework.** `authentication_manager` / `authentication_plugin`
  scaffold is never invoked on connect/publish; only the admin password file is live.
  No per-stream publish/play ACLs. Either wire it or prune it (see dead code).

## Dead / unwired code to prune or finish

- **`g711_codec.*`** — never instantiated; only Speex is wired to the mixer.
- **`authentication_manager.*` / `authentication_plugin.*`** — scaffold never called;
  the class is not instantiated anywhere in the tree, so `--auth-plugin` is an
  advertised option that cannot do anything (same shape as the inert queue options
  retired in 2.0.0 — either wire it or drop the option with the code). Couples with
  the "wire the auth framework" P3 item.
- RTMFP loose ends: `handle_flow_exception_report` (now a documented no-op),
  unfinished NetGroup re-notification.
