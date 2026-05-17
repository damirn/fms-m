# fms-m — feature gaps & backlog

Gap analysis vs reference implementations: **rtmpdump/librtmp** (RTMP client lib),
**zenomt/rtmfp-cpp** and **MonaSolutions/librtmfp** (RTMFP clients). "Gap" = a
protocol capability those exercise that our *server* can't serve or interop with.
File refs are indicative anchors, not exhaustive.

Legend: **[P1]** interop-breaking / high value · **[P2]** meaningful capability ·
**[P3]** nice-to-have / hardening · **[dead]** unwired code to prune or finish.

---

## RTMP (vs librtmp / rtmpdump)

- **[DONE — branch feat/amf3] AMF3 completeness.** Fixed the silent-corruption
  reference bugs (string ref returned `""`, object ref returned `{}`, broken
  traits-ref indexing) and added a proper per-message ref-table reset; fixed the
  integer sign mask (`0x18000000`→`0x10000000`, which had wrongly negated positive
  ints in [2^27, 2^28)); added Double, Date, ByteArray, Array (dense+assoc),
  XML/XMLDoc; externalizable now throws explicitly instead of mis-reading. Test
  suite under `test/` (doctest, `-DBUILD_AMF_TESTS=ON`): 19 cases / 108 assertions
  — amf-cpp-style exact-byte serialization vectors for every type (all U29 lengths,
  ±sign, special doubles, multi-byte string/bytearray lengths, empty/dense/assoc
  arrays, objects, xml), deserialization vectors, the reference-table + sign
  regressions, and malformed-input throws; fuzz harness (libFuzzer + standalone
  GCC/ASan) — 3M iterations clean. Remaining AMF3 follow-ups: reference-emitting
  *writer* (currently always inlines — spec-legal); Dictionary (0x11) + Vector
  (0x0D–0x10) types (post-2007, Flash 10+ — currently throw); IExternalizable
  class registry (rare); cross-validation fixtures from a reference codec (PyAMF).
- **[P1] No RTMPS / RTMPTS (TLS).** Plain TCP only; no `asio::ssl`. The only
  transport encryption is Adobe RTMPE (RC4), which is weak/legacy. Table-stakes
  gap. (`server.cpp:36-79`, `config.cpp:48-50`)
- **[DONE — branch feat/vod] `pause` / `pauseRaw` / `seek`.** Wired to the VOD
  engine: pause stops/resumes the pacing timer (Pause/Unpause.Notify), seek
  repositions via `flv_reader::seek` (Seek.Notify). The play command's start
  offset is honoured too.
- **[DONE — branch feat/vod] FMLE/OBS publish verbs.** `releaseStream`,
  `FCUnpublish`, `FCSubscribe` are acknowledged with `_result`; `FCPublish`
  replies `onFCPublish(NetStream.Publish.Start)` so FMLE-style encoders proceed.
- **[DONE — branch feat/amf3] AMF0 gaps.** Implemented Date (0x0B), XMLDocument
  (0x0F), TypedObject (0x10), Unsupported (0x0D), and Reference (0x07) with an
  AMF0 object reference table (anonymous/typed objects + arrays register; refs
  resolve to the same instance); previously these threw. Tests in
  `test/amf0_test.cpp` (shared doctest main): exact-byte serialization +
  deserialization vectors for every handled type (number, boolean, string,
  object, null, undefined, reference, ecma/strict array, date, long string,
  unsupported, xml-document, typed object, amf3 container) + round-trips + the
  reference-table regression + malformed-input throws. Fuzzer now covers both
  AMF0 and AMF3 (3M iterations clean under ASan). Not implemented (reserved):
  MovieClip (0x04), Recordset (0x0E) — still throw.
- **[P3] Abort Message (0x02) not implemented** — hits `default: return false`
  (`rtmp_protocol.cpp:55`).
- **[P3] Can't *require* SWF verification / SecureToken** (Adobe anti-leech).
  Not an interop break; a missing access-control feature.

Parity is good on: simple **and** FP9 digest/HMAC-SHA256 handshake, RTMPE (DH+RC4),
RTMPTE, RTMPT tunnel, chunk protocol control (set-chunk-size, ack window, set peer
bw, user-control, extended timestamps), server-side bandwidth check.

## RTMFP (vs rtmfp-cpp / librtmfp)

- **[~] librtmfp "Bad RTMFP CRC" — ROOT-CAUSED: it's a librtmfp bug, NOT ours.**
  librtmfp's `RTMFP::Engine::decode` (`sources/RTMFP.cpp:111`) never calls
  `EVP_CIPHER_CTX_set_padding(_context, 0)`, so on decrypt OpenSSL 3's
  `EVP_DecryptUpdate` **holds back the final 16-byte block** (PKCS7 still on) while
  it checksums the full buffer → 16 bytes of stale ciphertext → false CRC fail.
  Our RHello is a valid packet: it decrypts cleanly and its checksum verifies both
  byte-orders (rtmfp-cpp accepts it). Proven by a standalone C repro *and* by
  adding that one line to librtmfp — after which it connects to our **unchanged**
  server ("RTMFPSession is now connected"). No fms-m change needed. Patched
  librtmfp `.so` on the box (backup at `/tmp/RTMFP.cpp.bak`) for future testing.
- **[P2] RTMFP NetConnection app resolution differs from librtmfp.** Once the CRC
  bug is patched, librtmfp reaches connect but gets `NetConnection.Connect.
  InvalidApp` for `bcast` (rtmfp-cpp connects fine). Our RTMFP→app mapping doesn't
  match the app string librtmfp sends — a higher-layer interop nuance to chase
  next. *(new, discovered while root-causing the CRC issue)*
- **[P2] DH group 2 (1024-bit) only.** Modern clients default to group 14
  (2048-bit) and must negotiate down. `evp_dh` is group-agnostic but only ever
  called with the one prime. (`dh2.cpp`, `service.cpp:17-23`)
- **[P2] Congestion/flow control is crude.** RTT estimation exists, but only a
  6-packet burst cap; `m_outstanding_bytes` unused, receive window hardcoded
  `0x7f`, `m_prev_rwnd` never populated (`session.cpp:555,587`). Poor on
  lossy/high-BDP links.
- **[P2] NetGroup is a skeleton** — join + peer-list dissemination only; no
  posting/ranking/gossip multicast, no group media relay (`group.h`,
  `session.cpp:418`). P2P is introducer-only (`redirect_ihello`), no P2P media.
- **[P3] Crypto verification is lax:** IIKeying signature parsed but never
  verified (`chunk.cpp:122`); cookie is a plaintext addr/port/ts compare, not
  HMAC'd (`service.cpp:390`).
- **[P3] No FEC.**
- **[P3] Half-open session leak** — `// fixme: stalled initial sessions should be
  removed too` (`service.cpp:173`).

## Media / server features (vs modern servers — nginx-rtmp / SRS context)

- **[DONE — branch feat/vod] VOD playback of saved files.** When a `play` target
  has no live publisher, the server serves a saved `.flv` from the output folder
  via a timer-paced "virtual publisher" (`vod_session` + `flv_reader`). Stream
  names are resolved through `resolve_media_file` (`media_path.h`), which blocks
  path traversal / absolute paths / escapes (unit-tested); `flv_reader::seek`
  supports seeking. Verified end-to-end (ffmpeg plays a saved file; traversal
  attempts serve nothing).
- **[P2] No HLS / DASH / fMP4** — FLV recording is the only output container.
- **[P2] No native relay** (push-to-remote / pull-from-origin). "Pull" is an
  external `execvp`'d helper (`spawn_helper`). No edge/origin clustering.
- **[P3] Auth framework unwired** — `authentication_manager`/plugin scaffold is
  never invoked in the connect path; only the admin app's password file is live.
  No per-stream publish/play ACLs.

## Dead / unwired code to prune or finish

- ~~**[dead]** `flv_reader.*` — no VOD, never referenced.~~ Now wired for VOD.
- **[dead]** `g711_codec.*` — never instantiated; only Speex is wired to the mixer.
- **[dead]** `authentication_manager.*` / `authentication_plugin.*` — scaffold
  never called.
- Numerous RTMFP `// fixme` markers (address-type enums, empty
  `handle_flow_exception_report`, unfinished NetGroup re-notification).

---

## Suggested first picks (bounded, high value)

1. **librtmfp "Bad CRC"** — smallest, most concrete interop win; may already be
   fixed by the earlier RTMFP interop work. *(in progress)*
2. **AMF3 completeness** — bounded, unblocks AMF3-encoding clients.
3. **RTMPS/TLS** — larger, but the headline transport-security gap.
