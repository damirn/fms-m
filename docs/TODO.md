# fms-m — backlog

Everything still open, in one place: the feature/interop gap analysis, the
structural debt, and the unfinished items from both code reviews
(`REVIEW_FINDINGS.md`, 2026-07; `REVIEW_2026-09.md`, 2026-09).

Gap analysis is vs reference implementations: **rtmpdump/librtmp**,
**zenomt/rtmfp-cpp** and **MonaSolutions/librtmfp**. "Gap" = a protocol capability
those exercise that our *server* can't serve or interop with. File refs are
indicative anchors, not exhaustive.

**Top-level goal:** 100% interop with rtmpdump + rtmfp-cpp, verified by the
automated matrices (`test/interop/interop.sh`, `test/interop/realworld.sh`).

Legend: **[P1]** interop-breaking, or a correctness/resource-safety defect ·
**[P2]** meaningful capability · **[P3]** nice-to-have / hardening.

Priority-sorted. Updated 2026-09-03.

> History lives in `git log`, not here. This file is forward-looking only.

---

## P1 — correctness / resource safety

- **Origin-pull helper spawning is unbounded and runs under the media lock.**
  `spawn_helper` is called from `handle_invoke_play` while the registry's EXCLUSIVE
  lock is held. There is no dedup, rate limit or cap, so repeated plays spawn
  repeatedly; and the origin URL is fully client-supplied, making the server issue
  outbound connections to arbitrary hosts. All gated on `--helper-app` being
  configured (off by default), which is the only reason this is not urgent.
  (`remote_relay.cpp`, `media_application.cpp:388`)
  *Reduced 2026-09:* the spawn is `posix_spawnp` now, so it no longer copies the
  server's page tables, and a failure is reported instead of silently ignored. The
  lock scope, the missing cap and the SSRF surface are unchanged.

## P2 — meaningful capability

### RTMP (librtmp / rtmpdump)

- **Enhanced RTMP (E-RTMP) FourCC codecs.** The single largest capability gap, and
  the only open item that changes what the server can do. librtmp + rtmfp-cpp speak
  the codec extension: video HEVC/AV1/VP9/VP8/VVC, audio Opus/AC3/E-AC3/FLAC, the
  `_EX` messages (COMMAND_EX 0x11, DATA_EX 0x0f, SHAREDOBJ_EX 0x10), and multitrack.
  Config-frame caching assumes AVC/AAC layout, so modern OBS HEVC/AV1 mis-caches.
  **Confirmed absent end to end (2026-09):** ffmpeg 8.0 muxes both HEVC and AV1 into
  FLV with E-RTMP signalling, the server accepts the publish and nothing plays back;
  it does not crash or wedge on either. Reproduction: cases a5/a6 in
  `test/interop/realworld.sh`. Plan: `docs/enhanced-rtmp-plan.md`. Spec:
  `docs/reference/enhanced-rtmp-v2.md`. Pass-through parity at minimum.
  — review N5

- **Resume-on-republish for persistent subscribers.** On unpublish FMS keeps the
  subscriber connected and re-parks it as a waiting client (re-arming the
  buffer-empty flag so a republish re-emits BufferReady(32)); fms-m tears the
  subscriber down. rtmpdump and ffmpeg both close anyway, so this only affects
  persistent clients. (`stream_registry::remove_broadcaster`, the waiting-client
  promote path)

- **`StreamDry`(2) is never emitted.** The constant exists (`rtmp_message.h:224`)
  and nothing sends it.

- **SWF verification enforcement (SecureToken).** Support *requiring* SWFVerification
  so `rtmpdump --swfVfy` interops and anti-leech works. Not implemented.

- **play2 / playlist + reset semantics.** librtmp's play2/switch and
  `NetStream.Play.Reset` transitions; rtmpdump playlist / `--start` resume. Not
  implemented.

### RTMFP (rtmfp-cpp / librtmfp)

- **DH group 14 (2048-bit).** Implement group 14 so we don't depend on clients
  negotiating down to group 2; full FlashCrypto parity. `evp_dh` is group-agnostic
  but only ever called with the group-2 prime. (`dh.cpp`, `rtmfp/service.cpp`)

- **Robust flow / congestion control.** `m_outstanding_bytes` tracking and correct
  retransmit under loss. *Done when:* a multi-MB `tcpublish→tcconn` transfer
  completes intact under simulated loss. (`rtmfp/session.cpp`)

- **NetGroup / P2P parity.** Full group semantics + peer introduction so rtmfp-cpp
  P2P (`tcconn` group mode) interops, beyond today's join + peer-list skeleton. No
  group media relay; P2P is introducer-only. (`rtmfp/group.h`, `rtmfp/session.cpp`,
  `redirect_ihello`)

- **Chunk types we don't parse** (RFC 7016): **FRAGMENT (0x7f)** packet-level
  fragmentation for over-MTU packets (long control/data packets break without it);
  **ACK_BITMAP (0x50)**; **BUFFERPROBE (0x18)**; **ECN_REPORT (0xec)**;
  **RHELLO_COOKIE_CHANGE (0x79)**.

- **librtmfp app-resolution needs a real librtmfp run to close.** Root-caused and
  fixed server-side: app matching strips a leading `/` and a trailing `?query`, so
  the raw RFC-3986 path `/media` librtmfp sends resolves. Proven via `rtmpdump -a`
  (interop case 6) and unit-tested (`test/match_app_test.cpp`), but librtmfp itself
  is not available locally. Note: librtmfp's "Bad RTMFP CRC" was root-caused as
  *their* OpenSSL-3 padding bug — no fms-m change needed.
  (`util::match_app_name`)

### Media / server

- **Shared-object scope + persistence.** Reclaiming an object when its last client
  goes matches FMS's NON-persistent objects. Two FMS behaviours we lack, both
  optional: a client may request **persistence**
  (`SharedObject.getRemote(name, uri, true)`), committing to `StorageDir`
  (`AutoCommit` on by default) so it survives instance unload and restart; and FMS
  **bounds** them — `MaxSharedObjects` 50000 per vhost, plus
  `MaxProperties`/`MaxPropertySize` per object. We cap nothing. On FMS the app
  instance is the unit of both isolation and lifetime here, which ties to the item
  below.

- **App instances are parsed but not honoured.** `util::match_app_name` splits
  `media/roomA` into app + instance and it is stored on the session
  (`client_session::m_app_instance`) and threaded through
  `delete_connection(conn_id, app_instance)` — but nothing on the media path scopes
  by it: `add_broadcaster`/`broadcaster_for_name` use the bare stream name and
  `so_manager` is instance-agnostic. So `rtmp://host/media/roomA/x` and
  `rtmp://host/media/roomB/x` are the same stream here and different streams on FMS.
  The sole exception is `video_call_application`, which does scope by instance
  (`m_instance_to_client`) — so the plumbing is proven, just not applied to streams
  or shared objects. Either honour the instance or stop accepting it silently.
  (`connect_router.cpp`, `stream_registry.h`, `so_manager.*`)

- **HLS / DASH / fMP4 output.** FLV recording is the only output container.

- **Native relay** (push-to-remote / pull-from-origin). "Pull" is an external
  spawned helper today; no edge/origin clustering.

### Infra

- **Interop matrix — remaining cases.** Two harnesses exist: `interop.sh` (protocol
  shape: which user-control events are sent and consumed) and `realworld.sh` (codec
  matrix, concurrency, transports carrying media, durability, limits). Still open:
  **RTMPE/RTMPTE** (blocked on the rtmpdump ARM bus-error — try a Linux runner or
  librtmfp; note the handshake path itself is now unit-tested, see
  `test/handshaker_test.cpp`), an **`fcclient` raw-handshake** case, and a
  **loss/congestion** RTMFP transfer (ties to the flow-control P2 item).
  A **slow-consumer** case landed in `realworld.sh` (F1).

## P3 — nice-to-have / hardening

### Testing

- **RTMFP `session`/`service` state machines still have no direct test.** Much of
  the tier is now covered — `rtmfp_parser_test`, `rtmfp_keying_options_test`,
  `rtmfp_flow_maps_test`, `rtmfp_session_id_test`, `rtmfp_chunk_test`,
  `rtmfp_flow_test`, `rtmfp_cookie_test`, `rtmfp_session_crypto_test` — but the
  packet/flow orchestration in `session.cpp` and the handshake state machine in
  `service.cpp` are reachable only through live sockets. Standing them up needs a
  service, an endpoint and an app manager; see the *testable seam* item below, which
  is the prerequisite. This is what makes the deferred RTMFP fixes unsafe to attempt.
  (`docs/concurrency.md`) — review T7

### Structural design debt

Priority order; the first item is the one the others hang off.

1. **`rtmp_application` is a god base class** (56 declarations). Every app inherits
   bandwidth-check, shared objects, result handlers, the async queue, the delay map
   and stats whether it uses them or not — `admin_application` needs none of the
   media machinery. Composition, not inheritance; the invoke string-ladder item is a
   symptom of the same thing.

2. **RTMFP is the risk concentration**: a second full protocol stack with its own
   lock-free model, reaching into `rtmp_app_manager` directly. Ties to the testing
   item above and the seam item below.

3. **Give the RTMFP transport a testable seam** (prereq for the testing item).
   `session` is both the RFC-7016 transport engine and the RTMP demux/dispatch (it
   includes `rtmp_protocol.h`/`rtmp_app_manager.h`), constructed only against live
   sockets/timers — which is *why* the tier has no state-machine tests. Introduce a
   narrow `flow_message_sink(stream_id, bytes)` boundary, split I/O binding
   (`open()`/`start()`) out of the constructors, inject the app-manager and a clock.
   (`rtmfp/session.cpp`, `rtmfp/service.cpp`)

4. **RTMFP chunk memory has an unwritten "view vs. copy" rule.** Some chunks hold
   `const uint8_t*` into the packet buffer that is freed when `parse()` returns; the
   same dangling-view bug has been patched repeatedly. `ihello_chunk`,
   `ping_reply_chunk` and `redirect_chunk` now own their copies; the rest still
   borrow. Parser should hand `unique_ptr<chunk>`, and any chunk that outlives the
   packet should own its buffer. (`rtmfp/chunk.h`, `rtmfp/parser.cpp`)
   — 2026-07 M13

5. **RTMFP control replies share one `m_ready_chunk` raw slot**, hand-freed across
   13 sites (already leaked once). Replace with an owning `deque<unique_ptr<chunk>>`
   drained in the send pass — also lifts the one-control-reply-per-packet limit.
   (`rtmfp/session.cpp`) The *redirect* queue beside it became
   `queue<pair<endpoint, unique_ptr<chunk>>>` (M30); this slot did not.

6. **Layer the codec / data model / framing (RTMP).** `rtmp_protocol::deserialize_*`
   is 15 boilerplate helpers shadowing the message `deserialize` virtuals;
   `byte_writer` fuses a serializer and an async-fill accumulator under debug-only
   asserts. Make the codec a free service over byte ranges (leaving `rtmp_message` a
   pure data object and `rtmp_protocol` a thin framing/factory layer) and split
   `byte_writer`. (`rtmp_protocol.cpp:15-61`, `byte_writer.h`)

7. **Replace the invoke string-ladders with per-app dispatch tables.** Every app plus
   the base re-implements an `if (fn == …)` chain, so a new verb edits a chain and
   every subclass carries base machinery it may not use. Register an
   `unordered_map<string_view, handler>` per app in its ctor.
   (`rtmp_application.cpp`, each app's `handle_invoke`)

8. **Finish the `get_connection` → `get_connection_opt` migration.** The throwing
   overload is still on 9 live call sites.

9. **Make `client/` an actual library.** The shipped `nc`/`ns_event_handler` are
   hard-wired to the CLI `config` singleton, so both real consumers (helper tool,
   b2b test) bypass them and reimplement handlers; sink ownership is raw-pointer
   with leak paths; `net_connection` is a 722-line god object. Split the CLI from
   the reusable core and export a typed status-code vocabulary (the
   `NetConnection.Connect.*` / `NetStream.*` strings are re-typed in ≥4 places).

10. **Inject `config` instead of reaching the singleton.** `config::instance()` is
    pulled from 15 files across every layer — a global that blocks unit-testing
    those classes in isolation. Pass the needed config in rather than reading the
    singleton in ctors.

11. **`rtmp_app_manager` still composes and dispatches** after the three-way split
    into `connect_router` + `connection_registry` + `netstream_stats_registry`.
    — 2026-07 M10

12. **Remaining `dynamic_pointer_cast` transport type-switches.** The rtmp endpoint
    is cached and the admin path went to transport virtuals; the rest are deferred.
    — 2026-07 M9

13. **Exceptions for routine "no such connection" lookups** → `optional`. Touches
    the central manager and every catcher. — 2026-07 M11

14. **Vestigial `boost/asio/detail/socket_ops.hpp` includes.** Four files include
    this Boost *detail* header and use nothing from it, left behind when the wire
    codecs moved to `byte_order.h`: `flv_writer.cpp`, `rtmp_header.cpp`, `amf0.cpp`,
    `byte_writer.h`. `byte_writer.h`'s comment still claims it needs
    `host_to_network_long`. Delete them.

### RTMP / RTMPT correctness (deferred — real clients don't hit these)

- **RTMP simple-handshake fallback** for a versioned C1 with no valid digest (all
  real clients sign; `build_response` refuses it today — see `handshaker_test`).
- **Partial handshake split across POSTs** (real clients send it whole). The
  out-of-order stash drain itself is now covered by `test/rtmpt_manager_test.cpp`;
  what is untested is the multi-connection reordering that produces the gap.
- **Shared Object codec: an unknown event type is not skipped.**
  `rtmp_message_shared_object::deserialize_event` reads an unknown event's type and
  32-bit length and then does not skip the body, so the next read starts mid-event
  and the whole message is refused. Safe (it throws, nothing is delivered) but not
  forward compatible: a peer sending any newer SO event type loses every event in
  that message. `buffer.skip(len)` in the `default:` arm would fix it; it changes
  wire behaviour, so it is a decision, not a cleanup. (`rtmp_so_message.cpp`)
  — review N6
- **librtmp connect-param coverage** (`auth`/`token`/`subscribe`/`tcUrl`/`swfUrl`/…)
  and command verbs we don't handle: `secureTokenResponse`, `set_playlist`/
  `playlist_ready`, client-initiated `_checkbw`, `onFCSubscribe`/`onFCUnsubscribe`.

### RTMFP correctness (deferred — needs the test seam above first)

- **`handle_flow_exception_report` is a documented no-op.** Peer-initiated
  sending-flow teardown needs coordinated removal across `m_sending_flows` +
  `m_flow_id_to_stream_id`/`m_stream_id_to_flow_id` + the owning app. The
  end-of-stream half of that lifecycle landed in 2026-09 (F5,
  `purge_stream_flows`); the peer-initiated half did not. (`rtmfp/session.cpp`)
- **Secondary echo-cookie random is never validated** → weak return-routability.
  Making it verifiable needs a keyed-HMAC scheme (protocol hardening).
  (`rtmfp/service.cpp`) — 2026-07 M3
- **One control-reply per received packet** (see structural item 5).
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
- **CI — nothing runs the tests.** There is no `.github/workflows`. Unit, fuzz, b2b,
  bench, and both interop matrices all exist and are run by hand. The Dockerfiles
  take `RUN_TESTS=1` and would make a reasonable CI job. A sanitized run is now worth
  wiring in too: `-DSANITIZE=address` / `=thread` sets `halt_on_error`, so a finding
  fails ctest instead of printing a warning into a green log.
- **Prometheus-style metrics endpoint** (admin stats exist over RTMP).

## Auth: wire it or prune it

Tracked as one item because the answer decides several others.

- **Nothing ever calls `authentication_manager`.** The class is not instantiated
  anywhere in the tree, so `--auth-plugin` is an advertised option that cannot do
  anything. Only the admin password file is live. There are no per-stream
  publish/play ACLs.
- **The plugin interface cannot express async auth.** `authenticate` returns a
  `tribool`, so a plugin can answer `indeterminate` and then has no way to complete.
  The manager's unused completion-callback parameter was removed rather than left as
  a promise the interface cannot keep. Async auth needs the interface changed first.
  — review F3
- **`client_session::set_username` is wired but only from the admin login.** The
  authenticated identity now reaches the session and `getClients` reports it
  (2026-09 F2). A media-path auth plugin would be the other producer.

Either wire the framework (interface change first) or prune it and drop the option
with the code.
