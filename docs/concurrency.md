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

## Lock order

The invariant above removes locks *within* one connection. State shared *across*
connections still needs them, and there are about a dozen such domains. Two rules
keep the graph shallow enough to reason about.

**Rule 1 — release before calling out.** A lock is not held across a call into
another subsystem. This is what stops most of the pairs below from ever being held
together, and it is deliberate at each site:

| Site | Releases before |
|------|-----------------|
| `connection_registry::delete_connection` | calling `app->delete_connection` (`lock.unlock()` first) |
| `so_manager::handle_so` | the fan-out — sends are collected into `pending_sends_t` under the lock and flushed after |
| `rtmp_application::enqueue_async_message_unchecked` | `add_dropped_messages_for_netstream` / `destroy_connection` |
| `rtmpt_manager::handle_data` | the per-session parse/dispatch — global lock is for the id table + sequencing only |
| `netstream_stats_registry::remove`/`remove_all`/`update` | notifying the observer |

**Rule 2 — when two must be held, this is the order.** Acquire left to right; never
the reverse:

```
stream_registry::m_mutex          (media routing; the app's "big" lock)
  └─> vod_manager::m_mutex        (start/stop/stop_connection are caller-locked)
  └─> rtmp_application queues     (m_async_map_mutex ─> async_queue::mutex)
  └─> connection_registry::m_mutex
  └─> netstream_stats_registry::m_mutex

rtmpt_manager::m_mutex            (id table + sequencing)
  └─> rtmpt_session_data::m_session_mutex
```

Everything not related by an arrow is never held together. In particular
`connection_registry`, `netstream_stats_registry`, `so_manager`,
`result_handler_registry` and `admin_application::m_admin_mutex` are leaves: they are
taken, used, and released without acquiring anything else.

Notes on specific edges:

- **`stream_registry` first, always.** The per-frame data path takes it shared and
  then calls into the manager (`update_netstream_stats`, `get_connection`). No
  manager method may call back into an application while holding a manager lock —
  `delete_connection` unlocks first precisely so this stays true.
- **`vod_manager` is *not* under the registry lock for its own work.** `tick()`,
  `pause()`, `seek()` and `set_buffer_length()` take only `vod_manager::m_mutex`;
  they touch no registry state. `m_mutex` is *not* a leaf, though: `tick()` holds it
  across `app_host::enqueue`, `send_status` and `notify_connection`, so the
  application's async-queue locks sit under it. Only `start()`/`stop()`/`stop_connection()` are
  called with the registry lock already held, which is the one place the edge above
  applies. This is why the per-frame VOD disk read no longer stalls live fan-out.
- **`rtmpt_manager`'s two locks** are the only pair held simultaneously by design,
  and only by the reaper and `remove_session`. The request paths take the global
  lock, release it, then take the per-session one — never both.
- **The queue pair** (`m_async_map_mutex` shared, then the per-connection
  `async_queue::mutex`) is always taken in that order; the map lock is a
  `shared_mutex` so enqueues to different connections do not serialise.

When adding a lock, put it in this table before writing the code. If a new edge
would point right-to-left against the order above, that is the design problem, not
the ordering.

## Consequence for shutdown

`server::stop()` calls `io_context_pool::stop()`, which `stop()`s each context and
**abandons** any queued handlers rather than running them to completion. Teardown
relies on handlers being dropped, not run — so a "graceful drain" (run every queued
frame to completion before exit) is *not* safe to bolt on without first cancelling
the acceptor and the app/manager timers. See the note at `server::stop()` and the
backlog item in `docs/TODO.md`.
