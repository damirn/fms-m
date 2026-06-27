# Concurrency model

The server is almost entirely **lock-free**. That is safe only because of one
invariant, stated here as a contract because the rest of the code depends on it
without re-checking it.

## The invariant: one thread per `io_context`

`io_context_pool` owns *N* `io_context` objects and, in `run()`, spawns **exactly
one thread per `io_context`** — never more. So every completion handler posted to a
given `io_context` runs on a single, fixed thread. There is no data race between two
handlers on the same `io_context`; they are serialised by construction.

`get_io_context()` hands out contexts round-robin. Once an object is associated with
a context it **stays** on it for its whole life — objects are never migrated between
contexts.

The guarantor is `io_context_pool::run()` (`io_context_pool.cpp`). If anyone ever
runs an `io_context` on more than one thread, or moves a live connection/session to a
different context, everything below silently breaks.

## What rests on it

- **RTMP/RTMPE/RTMPT connections.** A connection is pinned to one `io_context`, so
  `rtmp_connection`'s `m_state`, `m_write_in_progress`, and I/O buffers are touched by
  only that one thread — the read, write, notify, and timer handlers never overlap.
  No lock is taken on the per-connection hot path. (`rtmp_connection.h/.cpp`,
  `basic_rtmp_connection`.)

- **RTMFP service and sessions.** The whole RTMFP `service` is pinned to a single
  `io_context` (`server::init_rtmfp_service`). Its session/group maps
  (`m_initial_sessions`, `m_session_map`, `m_sessions`, `m_groups`) and each
  `session`'s flow maps carry no mutex, because every UDP packet for the service is
  handled on that one thread. (`rtmfp/service.h`, `rtmfp/session.h`.)

- **`rtmp_app_manager`'s `shared_mutex`.** The manager's connection map *does* take a
  lock — but only because it is read/written across contexts: the admin app (on its
  own connection's thread) reads connection/stats data that connection threads write.
  The lock guards *cross-`io_context`* access, not intra-connection races. The
  netstream stats live behind their own mutex in `netstream_stats_registry` for the
  same reason.

## Crossing threads deliberately

When one context genuinely must touch an object owned by another (e.g. the admin app
killing a connection, or a VOD/relay hand-off), it does **not** reach in directly. It
`boost::asio::post`s a closure onto the owning context so the work still runs on that
object's one thread. `basic_rtmp_connection::post_close()` is the canonical example:
it posts `close()` back onto the connection's context rather than closing the socket
from the admin thread.

## Consequence for shutdown

`server::stop()` calls `io_context_pool::stop()`, which `stop()`s each context and
**abandons** any queued handlers rather than running them to completion. Teardown
relies on handlers being dropped, not run — so a "graceful drain" (run every queued
frame to completion before exit) is *not* safe to bolt on without first cancelling
the acceptor and the app/manager timers. See the note at `server::stop()` and the
backlog item in `docs/TODO.md`.
