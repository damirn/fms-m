# RTMP / AMF parsing model

Design notes on how incoming data is parsed, why exceptions are used the way they
are, and where the current model is fragile. This is developer-facing rationale,
not user documentation (see `../README.md` for that).

## How it works today

Parsing is driven by `rtmp_raw_data::parse_data(stream_array &buffer)`, called
each time bytes arrive from the socket. It is a **hybrid** of length-based
framing and exception-based unwinding:

1. **Chunk payload assembly — length-based (no exceptions).**
   RTMP is length-prefixed at the chunk/message level, and the code uses that:

   ```cpp
   size = h.message_length() - channel->data_size();
   size = std::min(size, m_chunk_size);
   if (size <= buffer.available()) {
       channel->add_data(buffer, size);          // consume this chunk
       if (channel->has_enough_data())
           handle_message(channel);              // full message -> dispatch
   } else {
       buffer.compact();                         // partial chunk -> wait
       return;                                   // (no throw)
   }
   ```

   So the frequent case — a large media message arriving across many TCP
   segments — never throws. It compacts the buffer and waits for the next read.

2. **Chunk header + AMF reads — optimistic parse + `buffer_eof_exception`.**
   `stream_array::operator>>` / `read()` throw `buffer_eof_exception` when
   `available() < needed`. `parse_data` wraps the loop in:

   ```cpp
   buffer.mark();
   channel->deserialize_header(buffer);   // may throw buffer_eof if header split
   ...
   catch (buffer_eof_exception &) { buffer.rewind(); }   // restore m_read, wait
   ```

   Chunk headers are ≤ 12 bytes and rarely split across reads, so the throw +
   `rewind()` cost is negligible.

3. **Two other exception types** (`amf0_read_exception`, `amf3_read_exception`)
   are thrown by AMF value parsing and are caught in
   `rtmp_protocol::deserialize`. `buffer_eof_exception` is deliberately **not**
   caught there — it must propagate up to `parse_data` so the loop can rewind.

## What's good about it

- **The hot path avoids exceptions.** Length-based chunk assembly means the
  common "message split across reads" case is handled by a length check and
  `compact()`, not by throwing through a deep parse stack. This sidesteps both
  the per-fragment exception cost and an O(N²) re-parse-from-scratch trap.
- **Exceptions are confined to rare cases** (a chunk header that happens to be
  split, or malformed AMF), where their cost doesn't matter.
- The parser reads optimistically, so field-by-field `available()` checks don't
  clutter the deeply recursive AMF code.

## Where it's fragile

### 1. `buffer_eof_exception` has two meanings

The same exception signals both:

- **"the network hasn't delivered the rest yet"** (recoverable → rewind + wait),
  and
- **"an AMF length field points past the buffer"** (a *malformed* message that
  should be rejected).

The single `catch` treats both as "wait for more." For a **complete-but-
malformed** message (all bytes present, but an internal length lies), waiting
cannot help — at best it re-parses, at worst it can wedge the connection. This is
a robustness / DoS-shaped concern. It has not been reproduced with a test; the
concrete probe would be a message whose AMF length exceeds its own
`message_length`.

### 2. The "which exception propagates where" contract is invisible

Three exception types with different catch-vs-propagate rules, enforced only by a
comment in `rtmp_protocol.cpp`:

> *an amf3_read_exception escapes to the io_context worker thread and
> std::terminate()s the whole server (remote DoS). buffer_eof is deliberately NOT
> caught here — parse_data needs it to rewind.*

Getting this wrong once was already a remote-crash DoS (an uncaught
`amf3_read_exception`). A contract this subtle, guarded only by a comment, is
likely to break again during maintenance.

### 3. `mark()` / `rewind()` only restore `m_read`

Unwind correctness depends on every throw site between `mark()` and the length
check being safe to **re-run** from the mark — no committed side effects
(half-updated channel header, half-built object, stray allocation). This holds
today but is an invariant nothing enforces.

## Suggested direction (not yet done)

Separate the two conditions that `buffer_eof_exception` currently conflates:

- **Chunk assembly** stays length-based (already the case). This is the only true
  "need more network data" path.
- **AMF / message parsing** runs against the message's own **bounded** buffer
  (its `message_length` is known), and an overrun becomes a distinct
  **`malformed_message`** error → drop the message / close the connection, rather
  than rewind-and-wait.

That collapses the fragile three-way exception contract into:

- `need_more_data` — thrown only by the chunk-header parse, caught only by the
  loop; and
- `malformed_message` — thrown by message/AMF parse, caught → reject.

With that split, the "an AMF exception escaped and terminated the server" class of
bug becomes structurally impossible, and incomplete-vs-malformed is no longer
ambiguous.

The length-based core is sound and worth keeping; the one high-value change is
removing the `buffer_eof_exception` double meaning.
