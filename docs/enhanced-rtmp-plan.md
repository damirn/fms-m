# Enhanced RTMP (E-RTMP v2) — implementation plan for fms-m

Spec: `docs/reference/enhanced-rtmp-v2.md` (Veovera Enhanced RTMP v2).

## Framing: what actually changes

E-RTMP **adds no new RTMP message-type IDs** — it keeps audio (8), video (9),
and data/AMF (18). The entire enhancement lives in the **tag header/body format**,
selected by the high bit of the tag's first byte:

- **Legacy** first byte: `(frameType << 4) | codecId` (video) /
  `(soundFormat << 4) | rate|size|type` (audio).
- **Enhanced** (`IsExHeader`, bit 7 set): `1 | frameType[3] | packetType[4]`,
  then an optional **ModEx** loop, a **4-byte FOURCC** codec id, and for
  multitrack an `AvMultitrackType` + `trackId`. This is `ExVideoTagHeader` /
  `ExAudioTagHeader`.

Enums we must know:
- `VideoPacketType`: SequenceStart=0, CodedFrames=1, SequenceEnd=2, CodedFramesX=3,
  Metadata=4, MPEG2TSSequenceStart=5, Multitrack=6, ModEx=7.
- `AudioPacketType`: SequenceStart, CodedFrames, …, MultichannelConfig=4,
  Multitrack, ModEx.
- `VideoFourCc`: `avc1 hvc1 av01 vp08 vp09 vvc1`. `AudioFourCc`: Opus, AC-3, E-AC-3,
  FLAC, AAC, … (`makeFourCc`).
- `AvMultitrackType`: OneTrack, ManyTracks, ManyTracksManyCodecs.
- `CapsExMask`: Reconnect=0x01, Multitrack=0x02, ModEx=0x04, TimestampNanoOffset=0x08.
- `FourCcInfoMask`: CanDecode=0x01, CanEncode=0x02, CanForward=0x04.

## Scope for fms-m (a relay/recorder, NOT a transcoder)

fms-m never decodes codec bitstreams — it caches sequence headers, gates late
joiners on a keyframe, fans out frames verbatim, and records to FLV. So "full
E-RTMP support" here means **correct framing pass-through**: parse enough of the
enhanced header to classify each packet (config / keyframe / coded / seq-end /
metadata / which track), then relay + record unchanged, and advertise support in
the connect handshake. Codec decode/encode is explicitly out of scope.

## Current blockers (where the AVC/AAC assumption is baked in)

1. `rtmp_message_video_data::get_codec()` / `get_frame_type()` read the **legacy**
   first-byte layout — wrong the moment `IsExHeader` is set. (`rtmp_message.h:474-496`)
2. `rtmp_message_audio_data::get_codec()` — same, legacy sound-format nibble.
3. Sequence-header detection is hard-coded AVC/AAC:
   `get_codec()==eAVC && data()[1]==0` (video), `eAAC && …` (audio).
   (`av_delivery.cpp:28,134-140`)
4. The stream caches exactly **one** `avc_config` + **one** `aac_config`
   (`stream_registry.h:74-75`) — no room for HEVC/AV1/Opus or per-track config.
5. Keyframe-gated join keys off legacy `eKeyFrame` (`av_delivery.cpp:63-64`).
6. The `connect` handler ignores `capsEx` / `videoFourCcInfoMap` /
   `audioFourCcInfoMap` and advertises nothing back.

## Phased plan

### Phase 0 — Enhanced tag-header parser (foundation; no behaviour change)
- New `ertmp_tag.{h,cpp}`: given a video/audio tag body, produce a normalized
  descriptor — `{ is_enhanced, fourcc | legacy_codec, frame_type, packet_type,
  track_id, is_config, is_keyframe, is_sequence_end, is_metadata, ts_offset_ns,
  body_offset }`. Handles the ModEx loop, FOURCC read, and the multitrack
  (`AvMultitrackType` + per-track FOURCC/`trackId`) layout.
- **Fail-closed** on unknown FrameType/PacketType/FourCc (spec requires controlled
  failure), and process the *whole* message (a video message may batch several
  packet types / tracks for one timestamp).
- Unit tests over hand-built byte vectors: legacy AVC/AAC, HEVC/AV1/Opus
  SequenceStart + CodedFrames + SequenceEnd, ModEx timestamp-offset, and each
  multitrack variant.

### Phase 1 — Single-track pass-through parity (the 80% win)
- Route every `get_codec()`/`get_frame_type()` call site through the Phase-0
  descriptor.
- Generalize the config cache: key stored sequence headers by
  `(media_type, track_id, fourcc)` instead of the single `avc_config`/`aac_config`
  slots; cache any SequenceStart packet regardless of codec; replay all cached
  configs to a late-joining subscriber.
- Generalize the keyframe join gate to `descriptor.is_keyframe`.
- FLV recording: confirm the enhanced tag bytes are written **verbatim** (they
  are, if we don't reinterpret), and that the FLV header codec flags don't lie —
  either widen them or leave the enhanced signaling to the tags.
- **Deliverable:** HEVC/AV1/VP9 video + Opus audio publish → subscriber receives
  config + keyframe + media; recording produces a valid enhanced FLV.

### Phase 2 — connect negotiation (capsEx + FourCcInfoMap)
- Parse the client's `connect` command object for `capsEx`, `videoFourCcInfoMap`,
  `audioFourCcInfoMap`; store the per-connection caps.
- In the `_result` connect response, advertise the server's `capsEx`
  (Multitrack | ModEx | TimestampNanoOffset as implemented) and
  `video/audioFourCcInfoMap` = `CanForward` for `"*"` (a pure relay forwards any
  codec).
- **Deliverable:** clients that gate on server-advertised support enable E-RTMP.

### Phase 3 — Multitrack
- Parse `VideoPacketType.Multitrack` / `AudioPacketType.Multitrack`
  (OneTrack / ManyTracks / ManyTracksManyCodecs), per-track FOURCC + `trackId` +
  per-track body sizes.
- Per-track config caching (Phase-1 cache already keyed by `track_id`), and
  per-track fan-out so a subscriber can select tracks.
- **Deliverable:** a multitrack publisher (OBS multitrack / ffmpeg) relays intact.

### Phase 4 — Metadata frame, MPEG2TS, ModEx offsets
- `VideoPacketType.Metadata` (per-frame AMF metadata, e.g. HDR/colorInfo) —
  forward, and cache the latest per track like a sequence header.
- `MPEG2TSSequenceStart` handling.
- `VideoPacketModExType.TimestampOffsetNano` — carry/forward the nanosecond
  offset (paired with the `TimestampNanoOffset` capsEx bit).

### Phase 5 — Reconnect Request + protocol versioning
- Reconnect Request (server-initiated `NetConnection` reconnect to a new URI) —
  optional server capability (`CapsExMask.Reconnect`).
- Enhanced `onMetaData` (advertise `videocodecid`/`audiocodecid` as FOURCC,
  multitrack metadata) on the served metadata.

## Testing

- **No FMS 4.5 oracle here** — the 2011 build predates E-RTMP, so there's no
  ground-truth capture. Drive tests from the spec + real E-RTMP clients:
  - **OBS Studio 30+** publishes HEVC/AV1 over enhanced RTMP.
  - **ffmpeg** recent builds mux enhanced FLV (HEVC) — verify the local build
    supports it before relying on it.
  - **rtmpdump does NOT speak E-RTMP** — it can't be the play client for the
    enhanced path (it can still exercise legacy AVC/AAC for regression).
- Add interop-matrix cases: enhanced-video (HEVC) publish → subscriber gets
  config+keyframe+media and a valid recorded FLV; multitrack relay; connect
  `capsEx`/FourCcInfoMap round-trip (assert the server's advertised map in the
  connect response).
- Unit tests (Phase 0) are the backbone — they don't need a live client.

## Ordering / value

Phases **0–1** deliver the bulk of the value (HEVC/AV1/Opus relay + record — the
modern-OBS interop the TODO calls out). Phase 2 unlocks clients that gate on
negotiation. Phases 3–5 are incremental completeness. Each phase is
independently shippable and testable.
