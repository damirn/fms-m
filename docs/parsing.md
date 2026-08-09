# RTMP chunk parsing

How received bytes become messages, and the one exception contract the path
relies on. Entry point: `rtmp_parser::parse(byte_writer &)` in `rtmp_parser.cpp`.

## The committed-offset model

`parse()` is **resumable and non-throwing**. A connection hands it whatever has
arrived so far; it frames as much as it can and consumes exactly that much.

It tracks a `committed` offset — the point up to which everything has been fully
processed and is safe to drop. A partial chunk header, or a chunk whose payload
has not fully arrived, ends the scan without advancing past it, so the unconsumed
tail stays in the buffer for the next call. `byte_writer::consume()` then drops
only the committed prefix.

The return value is a `boost::tribool` with three distinct meanings, and the
connection's read loop depends on all three:

| Value | Meaning |
|-------|---------|
| `true` | At least one complete message was assembled and dispatched. |
| `false` | Everything readable was framed, but no message completed. |
| `indeterminate` | Stopped on a partial chunk or header; call again with more bytes. |

`test/chunk_parser_test.cpp` pins all three.

## Where messages go

`rtmp_parser` owns no sockets and no connection identity. It drives a
`channel_manager` for per-channel header and reassembly state, and delivers what
it assembles through an injected `rtmp_message_sink`:

- `handle_message` — a decoded application message (invoke, audio, video, …)
- `handle_internal_message` — a protocol-internal one. Set Chunk Size updates the
  parser through `set_chunk_size`; Window Acknowledgement Size is connection
  accounting and is delivered to **both**.

That injection is what lets the parser be constructed and tested on its own with
a recording sink, which is what `chunk_parser_test` does.

## buffer_eof_exception

`buffer_eof_exception` means a read ran past the end of a buffer. Its meaning
depends on which buffer:

- **During framing**, `parse()` never lets it escape — an incomplete chunk is
  flow control, reported as `indeterminate`.
- **During `deserialize`** of an assembled message body, the buffer is complete
  by construction, so an over-read means the body is corrupt. `dispatch()` catches
  it and drops that message; the chunk stream itself stays in sync.

`rtmp_protocol::deserialize` catches `amf0_read_exception`, `amf3_read_exception`,
`std::bad_alloc` and `std::length_error` and drops the message, but deliberately
lets `buffer_eof_exception` through to the caller above.

## Framing guards

`framing_error()` latches when a chunk header abuses the framing, and the owning
connection drops the connection when it does. It is set by:

- a Set Chunk Size below 1 (the per-chunk read length would degenerate to 0);
- a header shrinking `message_length` below what is already buffered on that
  channel;
- a message longer than `eMaxMessageLength`;
- the channel-map cap (`channel_manager::open_channel`) — the basic header can
  address 65599 channels and they are never evicted, so peer-chosen ids go
  through `find_channel`/`open_channel` rather than the find-or-insert
  `get_channel` used for ids we pick.
