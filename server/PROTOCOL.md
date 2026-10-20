# UGREEN LED Network Service — Protocol v1 (Client Reference)

This document describes the wire protocol of `ugreen_leds_server` for people who
write clients or monitoring agents. It documents what is implemented (stages
1–3); design rationale and non-goals live in [ARCHITECTURE.md](ARCHITECTURE.md),
and server-side configuration (endpoints, access rules, limits) in
[examples/ugreen-leds-server.conf](examples/ugreen-leds-server.conf).

## 1. Transports and endpoints

The server listens on:

| Transport | Default | Config |
|---|---|---|
| Unix domain socket | `/tmp/ugreen_leds.sock` | `--socket <path>` / `unix_socket=` |
| TCP (IPv4) | `127.0.0.1:39001` | `--bind <ip> --port <n>` / `tcp_bind=`, `tcp_port=` |

Both listeners can be disabled independently (`--no-unix`, `--no-tcp`).
Remote TCP listeners must be configured explicitly by the operator.

There is **no in-band authentication or secret**: access control is entirely
server-side and based on the peer address (Unix socket ⇒ `local`, TCP ⇒ source
IP) matched against configured access rules. A client cannot choose an
identity; it gets the policy of its source address, or the connection is
denied at accept time (see [Error reference](#8-error-reference)).

## 2. Framing and limits

- One command per line. Lines end with `LF` (`\n`); a trailing `CR`
  (`CRLF`) is accepted and stripped.
- Maximum line length: **1,024 bytes**, excluding the terminator.
- Maximum unprocessed input per connection: **8 KiB** (the server retains
  incomplete lines between reads).
- Maximum queued output per connection: **32 KiB**.
- Tokens are separated by spaces and/or tabs. All values are decimal
  integers unless stated otherwise. There are no quoting or escaping rules —
  the protocol vocabulary is a fixed set of identifiers.

TCP is a byte stream: a single `send()` may carry several commands, and one
command may be split across several segments. The server handles both.
Commands on a connection are processed in the order they arrive, and
responses are emitted in the same order.

Exceeding the line or input limits, or letting the output queue overflow
(the client never reads), causes the server to **close the connection without
sending an error line**. Reserve error lines for command-level problems.

## 3. Message format and request IDs

Client commands:

```text
<VERB> <request-id> [argument...]
```

Server responses:

```text
OK  <request-id> <ACTION> [key=value ...]\n
ERR <request-id> <CODE> [key=value ...]\n
```

- `<request-id>` is a client-generated **unsigned 64-bit decimal integer**
  (0 … 18446744073709551615).
- IDs are scoped to a single connection and may overlap across connections.
- Every response echoes the request ID of the command it answers; use the ID
  to correlate responses. **Do not reuse an ID while its response is still
  outstanding.** IDs provide no replay protection and no deduplication
  across reconnects.
- `key=value` fields are space-separated, in a fixed order per response
  (shown in the command sections below). Parse them by key, not by position,
  and ignore fields you do not recognize for forward compatibility.
- The server itself uses request ID `0` for the `AUTH_FAILED` message it may
  send at accept time (before any of your commands have been sent).

## 4. Session lifecycle

`HELLO` **must** be the first command on every connection. Any other command
before `HELLO` produces `ERR <id> PROTOCOL_ERROR reason=hello_required` and
the server closes the connection.

```text
C: HELLO 1 1
S: OK 1 HELLO version=1 access=local
```

- `access` is `local` for Unix-socket clients, or the client's source IP for
  TCP clients. It identifies the policy that matched, not a username.
- The protocol version is currently `1`. `HELLO <id> <other-version>`
  produces `ERR <id> UNSUPPORTED_VERSION version=<n>` and the server closes
  the connection.
- After `HELLO`, the connection is authenticated for the rest of its life.
  There is no re-authentication; reconnect and send `HELLO` again.

## 5. Commands

LED identifiers: `power`, `netdev`, `disk1` … `disk8`. Which of these exist is
**backend-specific** — always query `CAPS` and never assume (a development
instance reports `power,netdev,disk1..disk4`; an 8-bay device may report more).
The keyword `all` targets every LED the server supports.

### 5.1 `CAPS` — capabilities

```text
C: CAPS 2
S: OK 2 CAPS version=1 leds=power,netdev,disk1,disk2,disk3,disk4 modes=off,on,blink,breath
```

`leds` is the comma-separated list of LED identifiers the server will accept;
`modes` is the fixed mode vocabulary.

### 5.2 `SET` — desired-state update

```text
C: SET <id> <led|all> [mode=<mode>] [rgb=<R>,<G>,<B>] [brightness=<0-255>]
                     [on_ms=<0-65535>] [off_ms=<0-65535>]
                     [priority=<0..2^31-1>] [reply=none]
S: OK <id> ACCEPTED generation=<n>
```

A `SET` is a **partial update**: only the supplied fields change the LED's
desired state; omitted fields keep their current values. At least one field
is required.

Field rules:

| Field | Value | Notes |
|---|---|---|
| `mode` | `off` \| `on` \| `blink` \| `breath` | |
| `rgb` | `R,G,B`, each `0`–`255` | no spaces, e.g. `rgb=0,255,0` |
| `brightness` | `0`–`255` | |
| `on_ms` | `0`–`65535` | on-time of one blink/breath cycle |
| `off_ms` | `0`–`65535` | off-time of one blink/breath cycle |
| `priority` | non-negative integer | see [Ownership and priority](#7-ownership-and-priority); omit to use your configured default |
| `reply` | `none` | suppress the `OK ACCEPTED` response; **errors are still sent** |

Timing validation: the sum of `on_ms` and `off_ms` must not exceed `65535`
(the hardware encodes the full cycle in 16 bits). When the effective mode is
`blink` or `breath` (the given `mode`, or `blink` when `mode` is omitted),
**both half-cycles must be non-zero**: the controller chip rejects blink or
breath commands with `on_ms=0` or `off_ms=0`. This applies both when both
fields are given in one `SET` and when a single-field update resolves to a
zero half-cycle against the retained field. Violations produce
`ERR <id> INVALID_ARGUMENT field=on_ms`.

Semantics:

- `ACCEPTED` means the update passed validation and changed the **desired**
  state. It does **not** mean the hardware write completed — use `FLUSH` for
  a barrier (see [Delivery semantics](#9-delivery-and-timing-semantics)).
- `generation` is an opaque, strictly increasing server counter. Do not
  assume it is per-LED or dense.
- `SET … all` is **atomic**: every supported LED must be authorized and
  preemptable, or the entire command is rejected with no partial changes.
- Ownership effects are described in [Ownership and priority](#7-ownership-and-priority).

Examples:

```text
C: SET 10 disk1 mode=on rgb=0,255,0 brightness=128
S: OK 10 ACCEPTED generation=1
C: SET 11 disk2 mode=blink on_ms=100 off_ms=100
S: OK 11 ACCEPTED generation=2
C: SET 12 power mode=off
S: OK 12 ACCEPTED generation=3
C: SET 13 all brightness=64
S: OK 13 ACCEPTED generation=4
```

### 5.3 `GET` — state query

```text
C: GET <id> <led> [source=desired|applied|hardware]
S: OK <id> GET led=<led> source=<src> mode=<mode> brightness=<b> rgb=<R>,<G>,<B> on_ms=<n> off_ms=<n> status=valid
```

`source` defaults to `desired` when omitted.

| source | Meaning |
|---|---|
| `desired` | Latest accepted logical state (what your `SET`s produced). |
| `applied` | Last state the server successfully wrote to the hardware. May lag `desired`. |
| `hardware` | An explicit controller readback, executed on the hardware worker and **rate-limited server-wide** (default 10 reads/s; excess requests get `ERR <id> RATE_LIMITED reason=hardware_read`). |

`status` is `valid` when the server holds a confirmed value for that source,
`invalid` when it does not (unknown or partially written hardware state is
never reported as confirmed).

`GET source=hardware` is asynchronous: the response arrives when the read
completes, not when the command is accepted.

### 5.4 `RESET` — release ownership

```text
C: RESET <id> <led|all>
S: OK <id> RESET
```

Releases ownership of the target LED (or all LEDs owned by this connection
for `all`) and resets its desired state to the server-configured **fallback
state**, allowing other connections to take control. Only the current owner —
or a connection with a strictly higher priority (see
[Ownership and priority](#7-ownership-and-priority)) — may issue `RESET`.

### 5.5 `FLUSH` — completion barrier

```text
C: FLUSH <id>
S: OK <id> FLUSH
```

Creates a barrier over the updates previously accepted **from this
connection**. It completes (`OK`) when all of them have either been superseded
(no hardware work remains) or had their hardware writes complete. If a
hardware write failed in the meantime, the barrier fails:

```text
S: ERR <id> HARDWARE_ERROR
```

`FLUSH` does not imply a hardware readback, and no simultaneous physical
update is guaranteed. Note that updates sent with `reply=none` are still
covered by the barrier (the server tracks your last activity generation
regardless of reply suppression).

## 6. State model

Per LED the server tracks:

- **desired state** — what clients have accepted into;
- **applied state** — what the hardware worker last wrote successfully;
- **fallback state** — server configuration, applied on `RESET` and when the
  owning connection disconnects;
- **ownership** — which connection controls the LED, at which priority.

`SET` never reads the hardware; it only mutates the desired state. The
scheduler coalesces desired vs. applied and issues the minimal hardware writes
(see [Delivery and timing semantics](#9-delivery-and-timing-semantics)).

## 7. Ownership and priority

Control of an LED is acquired **implicitly on the first mutating `SET`** and
is exclusive until released (`RESET` or disconnect). There are no leases,
heartbeats, or expirations.

- **Priority** is an integer where **values strictly closer to 0 are
  higher-priority** (`0` is highest). Each access rule assigns the client a
  *ceiling*: a `SET` may specify `priority=` but never a value strictly
  closer to 0 than that ceiling (violation: `FORBIDDEN … reason=priority`).
  Without an explicit `priority=`, the rule's default applies.
- If an LED is already owned, a mutating `SET` from a different connection is
  accepted **only if its priority is strictly closer to 0** than the current
  owner's. Otherwise it is rejected: `ERR <id> REJECTED led=<led> reason=priority`.
  A successful competing `SET` **preempts** ownership to the new connection.
- Equal priority never preempts.
- When the owning connection disconnects, all of its LEDs are released and
  restored to their fallback states.
- `SET … all` is all-or-nothing: if any target fails the authorization or
  priority check, nothing is changed. `RESET all` releases every LED this
  connection owns (LEDs owned by other connections are untouched).

Practical consequence for agents: two agents must not drive the same LED
unless one has a strictly higher priority than the other and the other
expects to be preempted.

## 8. Error reference

All errors are `ERR <id> <CODE> [fields]` lines. "Closes" means the server
closes the connection after sending the line.

| Code | Fields | When | Connection |
|---|---|---|---|
| `AUTH_FAILED` | `reason=no_matching_rule` | peer address matches no access rule (id `0`, sent at accept) | closes |
| `AUTH_FAILED` | `reason=connection_limit` | per-rule connection cap reached (id `0`) | closes |
| `AUTH_FAILED` | `reason=server_limit` | server-wide connection cap reached (id `0`) | closes |
| `UNSUPPORTED_VERSION` | `version=<n>` | `HELLO` with a protocol version other than 1 | closes |
| `PROTOCOL_ERROR` | `reason=hello_required` | any command before `HELLO` | closes |
| `INVALID_ARGUMENT` | `reason=syntax` | wrong token count, missing `key=value` form | stays open |
| `INVALID_ARGUMENT` | `field=<name>` | unknown field, duplicate field, or value out of range / invalid (e.g. `mode`, `rgb`, `brightness`, `on_ms`, `off_ms`, `priority`, `source`, `request_id`, `reply`) | stays open |
| `UNKNOWN_COMMAND` | `verb=<v>` | first token is not a known verb | stays open |
| `UNSUPPORTED_LED` | `led=<name>` | LED identifier not provided by the backend (parse-level check; a name like `disk7` on a 4-disk device is rejected here) | stays open |
| `FORBIDDEN` | `led=<led\|all>` | target LED outside your access rule's LED subset | stays open |
| `FORBIDDEN` | `led=<led\|all> reason=priority` | requested `priority=` closer to 0 than your rule's ceiling | stays open |
| `FORBIDDEN` | `verb=<VERB>` | verb not in your access rule's verb list | stays open |
| `REJECTED` | `led=<led\|all> reason=priority` | LED owned by another connection at an equal or higher priority | stays open |
| `RATE_LIMITED` | `reason=command_rate` | per-connection command rate exceeded (token bucket, default 500/s) | stays open |
| `RATE_LIMITED` | `reason=hardware_read` | server-wide hardware-read rate exceeded | stays open |
| `HARDWARE_ERROR` | — | `FLUSH` barrier failed due to a hardware write error | stays open |
| `HARDWARE_ERROR` | `led=<led>` | `GET source=hardware` read failed | stays open |

Framing violations (oversized line, input buffer overflow, output queue
overflow) close the connection **without** an error line.

## 9. Delivery and timing semantics

- **Coalescing**: the server applies updates in batches (default interval
  20 ms, server-configurable). Rapid `SET`s for the same fields collapse into
  a single hardware write using the newest values. Keep at most one in-flight
  batch; obsolete intermediate states never queue up.
- **State, not events**: a short "flash" expressed as `mode=on` followed by
  `mode=off` within one batch window may vanish entirely. For visible
  minimum-duration activity, use `mode=blink`/`mode=breath` (timed by the
  controller, not by the server) or hold the state long enough.
- **`ACCEPTED` is not "on the panel"**: physical application is asynchronous
  and not transactional; a multi-LED update may briefly show intermediate
  states. Use `FLUSH` when you need a barrier.
- **Failures retry**: a failed hardware write keeps the desired state,
  invalidates the uncertain applied fields, and the scheduler retries on the
  next tick. `FLUSH` is the way to observe persistent failure
  (`HARDWARE_ERROR`).
- **Zero color/brightness in off/on modes**: the controller acknowledges but
  ignores zero-valued color/brightness updates while the LED is `off` or `on`
  (the registers keep their previous value; verified on real hardware). Such
  updates are accepted into the desired state but are **not retried** — the
  value is re-programmed together with the next mode change to blink/breath,
  which the chip does apply. Until then `GET source=applied` /
  `GET source=hardware` may show the previous color/brightness while
  `GET source=desired` shows the requested zero.
- **Rate limits**: per-connection command rate (token bucket, burst = rate,
  default 500/s; `0` disables) and server-wide hardware-read rate (default
  10/s; `0` disables). Both are server configuration.

## 10. Client / agent implementation notes

1. **Connect, `HELLO`, `CAPS`, then work.** Re-query `CAPS` after reconnecting;
   the LED set is a server fact, not a constant.
2. **Keep reading the socket continuously**, even when you use `reply=none`.
   Errors are never suppressed, and an unread error can fill the 32 KiB send
   queue and get the connection dropped.
3. **Handle `ERR` per request ID.** Retryable policy errors (`REJECTED
   reason=priority`, `RATE_LIMITED`) may warrant backoff and retry;
   authorization errors (`FORBIDDEN`) mean the server's policy does not grant
   you that operation — do not retry in a tight loop.
4. **On reconnect, resend complete desired state** for every LED you intend
   to control: ownership is released on disconnect and your LEDs were restored
   to their fallback states. Do not replay obsolete activity flashes.
5. **Reconnect with bounded exponential backoff and jitter.** A closed
   connection (including silent framing closures) requires a fresh `HELLO`.
6. **Use `FLUSH` sparingly** as a barrier before state transitions you depend
   on (e.g. before a `RESET`, or before reporting "LED is now showing X").
   High-frequency activity updates should normally be fire-and-forget
   (`reply=none`), with periodic `GET source=applied` to verify health.
7. **One persistent connection per agent** is the intended topology; do not
   reconnect per command.
8. **Do not parse the server's log output.** This protocol is the only
   client-facing interface.

## 11. Example session

Unix socket, development defaults (fake backend, permissive local policy):

```text
C: HELLO 1 1
S: OK 1 HELLO version=1 access=local
C: CAPS 2
S: OK 2 CAPS version=1 leds=power,netdev,disk1,disk2,disk3,disk4 modes=off,on,blink,breath
C: SET 3 disk1 mode=on rgb=0,255,0 brightness=128
S: OK 3 ACCEPTED generation=1
C: FLUSH 4
S: OK 4 FLUSH
C: GET 5 disk1 source=desired
S: OK 5 GET led=disk1 source=desired mode=on brightness=128 rgb=0,255,0 on_ms=0 off_ms=0 status=valid
C: RESET 6 disk1
S: OK 6 RESET
```

The same exchange over a restricted LAN rule (LED subset + `GET`/`SET` verbs,
priority ceiling `10`) would additionally show:

```text
C: SET 7 power mode=on
S: ERR 7 FORBIDDEN led=power
C: SET 8 disk2 brightness=99 priority=0
S: ERR 8 FORBIDDEN led=disk2 reason=priority
C: SET 9 disk2 brightness=99 priority=20
S: OK 9 ACCEPTED generation=7
```
