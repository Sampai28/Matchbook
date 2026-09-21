# Market data protocol

The wire format between `matchbook-server` and any client that wants to follow a
book live. `ui/src/protocol/types.ts` mirrors this file, and there is a
conformance test on each side that fails if the two drift.

---

## Transport: Server-Sent Events

Market data goes out over SSE at `GET /stream/{symbol}`. Order entry stays on
the existing REST endpoints.

**Why not WebSocket.** Market data here is strictly unidirectional — the server
publishes, the client listens, and order entry already has a perfectly good
REST path with typed errors and status codes. A WebSocket would buy
bidirectionality nobody needs, and cost three things: a dependency (cpp-httplib
has no WebSocket support, so it would mean adding one or hand-rolling the
handshake, framing and masking), a reconnection strategy written by hand, and a
second error-reporting convention alongside HTTP's.

SSE is plain HTTP with a `text/event-stream` body. `cpp-httplib` already streams
chunked responses, so the server side is a content provider and nothing else.
The browser's `EventSource` reconnects automatically with backoff, and resumes
by sending `Last-Event-ID` — which maps directly onto this protocol's sequence
numbers.

The honest cost: SSE is text, so a binary format would be smaller, and browsers
historically capped concurrent SSE connections per origin at six over HTTP/1.1.
Neither matters for one book in one tab.

---

## Shape of a session

```
client                                server
  │                                      │
  ├── GET /stream/MBK ──────────────────►│
  │◄── event: snapshot  (seq = N) ───────┤   full book, once
  │◄── event: delta     (seq = N+1) ─────┤   incremental, forever
  │◄── event: delta     (seq = N+2) ─────┤
  │◄── event: heartbeat (seq = N+2) ─────┤   no state change
  │                                      │
  │  (client notices seq jumped N+2 → N+7)
  ├── GET /snapshot/MBK ────────────────►│
  │◄── snapshot (seq = N+7) ─────────────┤   resynchronise
```

Every message carries a monotonically increasing `seq`. A client that receives
`seq` out of order, or with a gap, must discard its local book and re-snapshot.

---

## Sequence numbers

`seq` is per symbol, assigned by the gateway, and increments by exactly one for
every message that changes the book. It is **not** the engine's internal event
sequence — the engine emits several events for one order (an ack plus three
fills), but the book only reaches one new state as a result, so the gateway
publishes one delta.

Heartbeats repeat the last `seq` rather than incrementing it. They exist to keep
proxies from closing an idle connection and to let a client distinguish "nothing
is happening" from "the connection is dead", and they carry no state change, so
consuming one must be a no-op.

`seq` starts at 1 for the first message on a freshly started server and does not
reset when a client reconnects. Two clients connected at different times see
different starting points and the same subsequent stream.

---

## Message types

All messages are JSON objects in the SSE `data:` field, with the message type
repeated in the SSE `event:` field so `EventSource.addEventListener` can
dispatch without parsing.

### `snapshot`

The complete book as of `seq`. Sent once on connect, and again on request via
`GET /snapshot/{symbol}`.

```json
{
  "type": "snapshot",
  "seq": 1041,
  "symbol": "MBK",
  "engine": "v3",
  "bids": [{"price": 100000, "quantity": 250, "orders": 3}],
  "asks": [{"price": 100100, "quantity": 180, "orders": 2}],
  "restingOrders": 412
}
```

`bids` are ordered by price **descending** (`bids[0]` is the best bid); `asks`
**ascending**. Both are truncated to the requested depth, default 50, max 500.

Truncation is why a delta for a price level outside the snapshot window is still
sent: the client keeps only the levels it knows about, and applying a delta for
an unknown level below the window is a no-op rather than an error. See
"Truncation and the depth window" below, which is the subtlest part of this
protocol.

### `delta`

One or more level changes and trades resulting from a single engine operation.

```json
{
  "type": "delta",
  "seq": 1042,
  "symbol": "MBK",
  "levels": [
    {"side": "BID", "price": 100000, "quantity": 180, "orders": 2},
    {"side": "ASK", "price": 100100, "quantity": 0,   "orders": 0}
  ],
  "trades": [
    {"price": 100100, "quantity": 70, "takerSide": "BUY"}
  ]
}
```

`levels` entries are **absolute replacements**, not increments. A level with
`quantity: 0` has been removed and the client must delete it.

Absolute rather than incremental on purpose: an increment that is applied twice,
or applied to a level the client got wrong, silently corrupts the book and the
error compounds. An absolute value is idempotent — replaying the same delta
twice yields the same state — so the only thing the client must get right is
ordering, which `seq` already guarantees.

`trades` is the tape. Trades print at the **resting** order's price, matching the
engine's semantics. `takerSide` is the aggressor's side.

Either array may be empty. A delta with both empty is never sent; that would be
a heartbeat.

### `heartbeat`

```json
{"type": "heartbeat", "seq": 1042, "symbol": "MBK"}
```

Sent when no book change has occurred for the heartbeat interval (default 5s).
Repeats the last `seq`. Consuming it must not change client state.

### `error`

```json
{"type": "error", "seq": 0, "symbol": "MBK", "code": "UNKNOWN_SYMBOL",
 "message": "no such symbol"}
```

Sent before the stream closes when the symbol does not exist. `seq` is 0 because
no state was ever established.

---

## Gap detection and recovery

The client tracks `lastSeq`. On each message:

| Condition | Meaning | Action |
|---|---|---|
| `seq == lastSeq` | heartbeat | ignore |
| `seq == lastSeq + 1` | in order | apply |
| `seq > lastSeq + 1` | **gap** | discard book, `GET /snapshot/{symbol}`, resume |
| `seq < lastSeq` | server restarted, or a stale reconnect | discard book, re-snapshot |

A gap is not an error condition to log and continue past. A client that applies
a delta across a gap has a book that is wrong in a way it cannot detect, and
every subsequent delta compounds it. Re-snapshotting is cheap; being silently
wrong about the market is not.

Gaps are expected rather than exceptional: SSE reconnects after a network blip,
the server drops messages to a slow consumer, a proxy buffers and truncates.
The recovery path is therefore normal operation and is exercised by a test, not
an emergency branch that has never run.

---

## Truncation and the depth window

The gateway publishes deltas for **every** level that changes, including levels
outside the snapshot depth the client received. This is deliberate and it is
where a naive implementation goes wrong.

Consider a client with depth 10. The 11th-best bid improves and becomes the 3rd
best. If the gateway suppressed deltas outside the window, the client would
never learn that level exists, and its ladder would show a stale 3rd-best bid
indefinitely.

So: the server sends everything, and the client decides what to keep. A client
maintaining a depth-10 view applies every delta into a full local book and
renders the top 10. The bandwidth cost is small — a delta is ~60 bytes — and the
alternative is a client-side book that is wrong precisely when the market moves,
which is when it matters.

`GET /snapshot/{symbol}?depth=N` still truncates, because the initial payload is
the one place size actually matters.

---

## Endpoints

| Endpoint | Returns |
|---|---|
| `GET /stream/{symbol}` | `text/event-stream`; snapshot then deltas forever |
| `GET /snapshot/{symbol}?depth=N` | one `snapshot` message as plain JSON |
| `GET /stats/engine` | live latency percentiles and counters for the UI stats panel |

`GET /stream` accepts `?depth=N` for the initial snapshot only; deltas are
unfiltered regardless.

### `GET /stats/engine`

Distinct from the existing `GET /stats`, which is a human-facing dump. This one
is shaped for the UI panel and is stable:

```json
{
  "engine": "v3",
  "submit": {"p50": 84, "p99": 210, "p999": 640, "count": 120411},
  "cancel": {"p50": 61, "p99": 150, "p999": 410, "count": 31002},
  "counters": {"ordersAccepted": 120411, "ordersRejected": 88, "fills": 40122},
  "streams": {"clients": 2, "messagesSent": 90411, "dropped": 0}
}
```

Percentiles are **nanoseconds**, measured around the engine call only — not the
HTTP round trip, which is orders of magnitude slower and would drown the signal.

`streams.dropped` is the count of messages not delivered to a slow consumer. Any
non-zero value means some client saw a gap and re-snapshotted.

---

## Threading

The engine has no locks and the HTTP server historically ran a single worker. An
SSE connection holds its worker for the lifetime of the stream, so a
single-worker server would stop serving the moment one browser connected.

The gateway therefore requires the server to run a multi-worker pool with a
mutex serialising every engine call. The engine remains internally
single-threaded and lock-free; the *server* serialises access to it. Streaming
threads hold the lock only long enough to copy a queued message, never while
writing to a socket.

---

## Versioning

There is no version field. This is a single-binary project where the server and
UI ship together, and a version negotiation nobody will ever branch on is
ceremony. The conformance tests on both sides are what keep them aligned; if
this ever shipped to third-party clients, a version field would go in the
snapshot message and the tests would gain a compatibility matrix.
