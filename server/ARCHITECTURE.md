# UGREEN LED Network Service — Proposed Architecture

Status: proposal, not an implemented interface.

## Review scope and recommendation

Use a persistent TCP service with a small, newline-delimited command protocol, one owner of the I²C controller, and a scheduler that coalesces rapid updates before writing to the hardware. This keeps the C++ implementation dependency-light while providing reliable communication between multiple clients.

Intended repository location: `server/ARCHITECTURE.md`.

## 1. Goals

Create a long-running C++ service, `ugreen_leds_server`, that allows local and remote clients to control the NAS front-panel LEDs.

The service should:

- Provide equivalents of the existing CLI's LED-control operations.
- Support several persistent client connections.
- Handle frequent updates without spawning processes or repeatedly opening the controller.
- Centralize hardware access and resolve conflicting client requests predictably.
- Keep dependencies limited to C++17, the standard library, and Linux/POSIX interfaces.
- Run under systemd on the machine that owns the I²C controller.
- Support clients in VMs, containers, and other machines without giving them direct hardware access.

The existing project documents a userspace CLI and a kernel-module route, which conflict with each other. This proposal uses the userspace controller route only.

Non-goals for the initial implementation:

- A web interface.
- HTTP, WebSockets, MQTT, or a broker.
- Remote execution of arbitrary CLI arguments.
- Arbitrary I²C/register access.
- Built-in disk-monitoring or filesystem-monitoring logic.
- Internet-facing operation without an external secure transport.

## 2. High-level design

```text
Disk activity agent ── TCP connection ──┐
Filesystem usage agent ─ TCP connection ┤
Administrative client ─ TCP connection ├── Network reactor
Local client ───── Unix socket ────────┘        │
                                              ▼
                                     Parser and validation
                                              │
                                              ▼
                                   Authorization and ownership
                                              │
                                              ▼
                                    Desired-state scheduler
                                              │
                                     bounded work handoff
                                              ▼
                                    Single hardware worker
                                              │
                                              ▼
                                   Existing LED / I²C backend
                                              │
                                              ▼
                                      NAS LED controller
```

The server is the sole userspace writer to the LED controller.

Clients submit desired LED state; they do not select an I²C bus, controller address, or register. Those are server-side configuration.

For Proxmox, run the service on the host owning the controller. Monitoring agents can run wherever the relevant data is visible—for example, inside a storage VM—and send LED updates over the network. The existing project likewise directs Proxmox users to run its hardware-accessing CLI on the host rather than in a VM.

## 3. Reuse of existing code

Initially compile the server against:

```text
server/ugreen_leds.cpp
server/ugreen_leds.h
server/i2c.cpp
server/i2c.h
```

Do not execute `ugreen_leds_cli` for each network request. Repeated process creation would undermine the purpose of a resident service.

Introduce a narrow backend interface:

```cpp
class LedBackend {
public:
    virtual ~LedBackend() = default;

    virtual Capabilities capabilities() const = 0;
    virtual LedState read(LedId id) = 0;
    virtual void apply(LedId id, const LedPatch& patch) = 0;
};
```

This interface is proposed, not an assertion about existing class names.

Provide:

- `I2cLedBackend`: wraps the current controller implementation.
- `FakeLedBackend`: records operations and injects failures for tests.

Before integration, inspect the existing implementation for:

- Error propagation and recovery behavior.
- File-descriptor lifetime.
- Unsupported-LED detection.
- Timing validation and overflow.
- Assumptions that are safe for a short CLI invocation but unsuitable for a daemon.

## 4. Transport decision

### Recommended: persistent TCP with line-delimited ASCII

Use one persistent connection per client.

Reasons:

- Reliable, ordered delivery.
- Straightforward implementation using POSIX sockets.
- No application-level retransmission or packet-ordering scheme.
- Easy testing with simple socket clients.
- Commands remain readable in logs and packet captures.
- No required JSON, HTTP, serialization, or networking framework.

TCP is a byte stream: one `recv()` may contain part of a command or several commands. The parser must retain incomplete lines and process all complete lines.

### Why not UDP initially?

UDP may be appropriate for a future “latest frame wins” stream, but it requires additional decisions about loss, reordering, authentication, stale updates, and control-message reliability.

Do not introduce those complexities until measurements show that TCP is insufficient.

### Why not a binary protocol initially?

Binary messages can reduce payload size, but LED-state messages are already small. A readable protocol is easier to implement, inspect, and extend.

If necessary, add a separately negotiated binary protocol later. Do not mix ambiguous text and binary framing on the same session.

## 5. Protocol version 1

### Framing and limits

- Each message ends with LF; optionally accept CRLF.
- Maximum line length: 1,024 bytes, excluding the terminator.
- Maximum receive buffer per connection: 8 KiB.
- Maximum queued output per connection: 32 KiB.
- Decimal unsigned integer fields.
- Fixed enumerated identifiers; no quoting or arbitrary strings in v1.
- Reject unknown commands, fields, duplicate fields, malformed numbers, and unsupported LEDs.
- Close connections that exceed framing or buffer limits.

All commands carry a client-generated request ID. IDs must not be reused while a response is outstanding. Request IDs are client-assigned unsigned 64-bit integers scoped to a single connection. Responses echo the corresponding request ID and are returned only on that connection. IDs may overlap across connections and may restart after reconnecting. Request IDs do not provide cross-session replay protection or deduplication.

### Proposed configuration
Use server-side rules associating a source address or range with a complete policy:

```
# Illustrative syntax, not an existing configuration format.

[access.admin]
source=192.168.178.30/32
leds=all
verbs=CAPS,GET,SET,RESET,FLUSH
priority=0
max_connections=2

[access.storage-agent]
source=192.168.178.120/32
leds=disk1,disk2,disk3,disk4
verbs=CAPS,GET,SET,RESET,FLUSH
priority=10
max_connections=2

[access.lan-status]
source=192.168.178.0/24
leds=all
verbs=CAPS,GET
priority=100
max_connections=2
```

The addresses above are examples, not assertions about where your agents run.

Priority values are integers where values strictly closer to 0 denote higher priority (`0` is highest priority).

Matching rules:

* No matching rule: deny the connection.

* Most-specific matching prefix wins: a /32 overrides a /24.

* The selected rule is the complete policy; do not merge permissions from broader rules.

* Reject duplicate rules for the same network during configuration validation.

* IPv4 and IPv6 policies are explicit and independently configured.

Thus, the example storage host gets its disk-control policy, not the broader LAN-status policy.

### Session initialization

Example:

Client connects from 192.168.178.120
```text
Client → HELLO 1 1
Server → OK 1 HELLO version=1 access=source-ip

Client → SET 2 disk1 brightness=128
Server → OK 2 ACCEPTED generation=1

Client → SET 3 power mode=off
Server → ERR 3 FORBIDDEN led=power

Client → RESET 4 disk1
Server → OK 4 RESET
```

`HELLO <request-id> <protocol-version>` must be the first command.

LED availability comes from the backend. Do not assume every device has eight disk LEDs.

Authentication failure returns an error and closes the session.

### LED updates

Examples:

```text
SET 10 disk1 mode=on rgb=0,255,0 brightness=128
SET 11 disk2 mode=blink on_ms=100 off_ms=100
SET 12 power mode=off
SET 13 all brightness=64
RESET 14 disk1
RESET 15 all
```

`SET` updates only the supplied fields; omitted fields retain their current desired values. The first connection mutating an unowned LED becomes its owner.

Supported fields:

| Field | Meaning |
|---|---|
| `mode` | `off`, `on`, `blink`, or `breath` |
| `rgb` | Three values in the range 0–255 |
| `brightness` | Value in the range 0–255 |
| `on_ms` | Blink/breath on duration |
| `off_ms` | Blink/breath off duration |
| `priority` | Integer priority; closer to 0 denotes higher priority (must not exceed configured access ceiling) |

The documented CLI accepts 16-bit timing arguments, while the controller protocol encodes total cycle duration and on duration as 16-bit values. Validate the combined cycle in a wider integer type and reject unrepresentable values rather than allowing wraparound. Exact zero-duration behavior must be verified against the backend before implementation.

`RESET <request-id> <target>` releases ownership of the target LED (or all owned LEDs if `all`), resets desired state to configured fallback, and allows other connections to take control.

For `all`, validate authorization and every target before accepting the update.

### Responses and completion

```text
OK 10 ACCEPTED generation=42
OK 14 RESET
ERR 11 INVALID_ARGUMENT field=on_ms
ERR 12 FORBIDDEN led=power
ERR 13 REJECTED led=disk1 reason=priority
```

`ACCEPTED` means the update passed validation and changed desired state. It does not mean hardware writes have completed.

Provide:

```text
FLUSH 14
```

`FLUSH` creates a barrier over updates previously accepted from that connection. It completes when those updates have either:

- Been superseded and require no remaining hardware work; or
- Had their resulting hardware writes complete successfully.

A failed write returns `HARDWARE_ERROR`. `FLUSH` does not imply hardware readback or simultaneous physical updates.

### Status

```text
GET 15 disk1 source=desired
GET 16 disk1 source=applied
GET 17 disk1 source=hardware
```

Distinguish:

- `desired`: latest accepted logical state.
- `applied`: last state successfully written by the server.
- `hardware`: an explicit controller read.

Include validity and pending/error information. Do not report unknown or partially written hardware state as confirmed.

Hardware reads go through the same hardware worker as writes; rate-limit them.

### High-frequency operation

Allow:

```text
SET 18 disk1 brightness=192 reply=none
```

This suppresses successful responses, not errors.

Clients must continue reading the connection even when suppressing success responses. Otherwise, errors can fill the output buffer and cause disconnection.

## 6. Multiple-client ownership

Control of an LED is acquired implicitly on mutation and resolved strictly by priority. The service does not use explicit leases, heartbeat renewals, or expiration timers.

```text
SET 20 disk1 mode=on brightness=128 priority=10
RESET 21 disk1
```

Policy:

- **Implicit ownership**: The first connection mutating an unowned LED becomes its owner and has exclusive control over subsequent updates.
- **Priority definition**:
  - Priority is represented as an integer where values **strictly closer to 0 denote higher priority** (with `0` being the highest priority).
  - Both configuration and protocol support priority settings:
    - **Configuration**: Each access rule assigns a default priority to matching connections.
    - **Protocol**: Mutating commands (`SET`) may optionally specify `priority=<N>`. Clients cannot claim a higher priority than allowed by their configuration rule (a value closer to 0 than configured is rejected as `FORBIDDEN`).
- **Conflict and preemption rules**:
  - If an LED is already owned, incoming mutations from a different connection require a priority check.
  - A mutating request from a competing connection is accepted only if its priority is **strictly closer to 0** than the current owner's priority.
  - If priority is not available (omitted from both command and configuration) or is not strictly closer to 0 than the current owner's priority (i.e., equal or lower priority), the `SET` command is rejected (`ERR <id> REJECTED led=<id> reason=priority`).
  - When accepted, the higher-priority connection preempts ownership and becomes the new owner.
  - Subsequent less-prioritized mutations are rejected until a `RESET` is issued by the higher-priority connection (or that connection disconnects).
- **RESET command**:
  - The current owner may relinquish control by sending `RESET <request-id> <led>`.
  - Upon `RESET`, ownership is cleared and the LED reverts to its configured fallback state, allowing other connections to control it.
  - A strictly higher-priority connection may also issue `RESET` to clear an LED held by a lower-priority connection.
- **Disconnect**:
  - When an owner connection disconnects, all LEDs owned by that session are automatically released and restored to their configured fallback states.
- **Multi-LED operations**:
  - Operations targeting multiple LEDs (such as `all`) are atomic and all-or-nothing: the connection must be able to acquire or preempt every target LED. If any LED fails the priority or authorization check, the entire command is rejected without partial state mutations.
- **Fallback state**:
  - On `RESET` or connection disconnect, apply a configured per-LED fallback state. Defaulting to a fallback prevents activity indicators from remaining indefinitely illuminated in an orphaned state.

## 7. Scheduling and rapid updates

Maintain per LED:

```text
desired state
last successfully applied state
desired generation
ownership and priority
dirty fields
pending/in-flight state
last hardware error
```

Suggested initial scheduler interval: 20 ms, configurable. This is a starting target, not a measured hardware capability.

Within each interval:

1. Merge accepted patches into desired state.
2. Retain the latest desired values rather than queuing every intermediate update.
3. Compare desired state with applied state.
4. Generate only necessary hardware operations.
5. Submit bounded work to the hardware worker.

For example, twenty brightness updates received within one interval should normally produce one write using the newest brightness.

Keep at most one in-flight hardware batch and one replaceable pending desired-state snapshot. This prevents obsolete updates from building an unbounded FIFO.

Differentiate state from events:

- State commands such as `SET` may be coalesced.
- Short activity flashes may disappear if a client sends `on` and `off` within the same interval.

A later `PULSE` command should provide server-timed, minimum-duration flashes. Until then, clients should use hardware blink mode or hold an activity state long enough to be visible.

Prefer controller-managed blink/breath over sending every transition over the network. The documented controller supports these modes directly.

The protocol accepts updates promptly, but it provides no hard real-time guarantee.

## 8. Threading and hardware consistency

Use two threads:

1. Network reactor:
   - Nonblocking sockets and `poll()`.
   - Parsing, authorization, priority-based ownership, and desired-state management.
   - Timers based on `std::chrono::steady_clock`.

2. Hardware worker:
   - Owns the backend.
   - Performs all controller reads and writes serially.
   - Reports completion using a bounded queue and wakeup descriptor.

Do not create one thread per connection.

The reactor must not hold a shared mutex while the worker performs hardware I/O.

A logical update may require several hardware writes. Validation is atomic, but physical application is not: intermediate colors or modes may briefly be visible. No transaction or rollback guarantee should be advertised.

On partial failure, preserve the desired state, invalidate uncertain applied fields, and report the error. Never treat a partially successful batch as fully applied.

## 9. Security and deployment

Default endpoints:

```text
127.0.0.1:39001
/run/ugreen-leds/server.sock
```

Port 39001 is a proposed configurable default, not a registered service assignment.

Remote listeners require explicit configuration.

Never expose raw register access, shell execution, or remotely configurable device paths.

Run the service in the foreground under systemd. Suggested deployment properties:

- Dedicated service account.
- Access to only the required I²C device node.
- `Restart=on-failure`.
- Runtime directory for the socket and process lock.
- Restrictive filesystem access.
- No unnecessary capabilities.
- Bounded shutdown timeout.

Device-node access grants access to the whole selected I²C adapter, not just one controller address. A dedicated account limits exposure but does not provide per-address isolation.

Determine the actual bus during deployment; do not hardcode the README's example bus number.

Use a process lock to prevent two server instances. It does not stop the existing CLI or another process unless those tools also participate in the same locking convention.

Do not automatically unload kernel modules or disable existing services. Document competing controllers and require the administrator to disable them explicitly. The project warns that its CLI and `led_ugreen` module conflict.

## 10. Monitoring clients

Keep telemetry collection outside the server.

Examples:

- Disk I/O activity: derive changes from block-device counters.
- Filesystem capacity: periodically calculate utilization.
- Storage health: map degraded/fault states to warning colors.
- Network activity: derive changes from interface counters.

“Disk usage” should remain a client policy: capacity utilization and I/O activity are different signals and may need different update rates.

Map physical disks to front-panel LEDs using explicit, verified identifiers rather than assuming `/dev/sdX` remains stable. The existing project documents serial-based mapping and warns that device names can change between boots.

Clients should:

- Maintain persistent connections.
- Send only changes.
- Issue `RESET` when relinquishing control of an LED.
- Reconnect with bounded exponential backoff and jitter.
- Re-authenticate and resend complete desired state after reconnecting.
- Avoid replaying obsolete activity updates.

## 11. Proposed directory layout

```text
server/
  ARCHITECTURE.md
  Makefile
  include/
    protocol.h
    led_state.h
    led_backend.h
    ownership.h
    scheduler.h
  src/
    main.cpp
    config.cpp
    protocol.cpp
    reactor.cpp
    ownership.cpp
    scheduler.cpp
    hardware_worker.cpp
    i2c_led_backend.cpp
  systemd/
    ugreen-leds-server.service
  examples/
    ugreen-leds-server.conf
    client.cpp
  tests/
    protocol_tests.cpp
    ownership_tests.cpp
    scheduler_tests.cpp
    fake_backend.h
```

Use C++17 explicitly and enable warnings. Do not force static linking solely because the CLI currently uses it; choose linking and runtime compatibility deliberately for the target host.

## 12. Implementation stages and acceptance criteria

### Stage 1: backend and parser

- Implement FakeLedBackend
- Implement framing, typed parsing, and strict validation.
- Confirm timing constraints and unsupported-LED behavior.
- Add fake-backend tests.

### Stage 2: local service

- Unix socket and loopback TCP.
- `HELLO`, `CAPS`, `SET`, `GET`, `RESET`, and `FLUSH`.
- Serialized hardware access and update coalescing.

### Stage 3: multiple remote clients

- Authentication and LED permissions.
- Priority-based ownership, preemption, RESET, and fallback states.
- Connection limits, rate limits, bounded buffers, and backpressure.
- Run stress tests to check against memory leaks or other system instability.

### Stage 4: deployment and performance

- systemd unit and example configuration.
- Persistent example client.
- Measure receive-to-apply latency, hardware operations per second, CPU use, and controller error rate.

### Stage 5: Wrap existing controller code for real LED HW
- Wrap existing controller code.
- Implement I2CLedBackend for real led HW
- Add tests specific to real led HW, incl. explicitly approved hardware smoke tests.

Tests must cover:

- Fragmented and combined TCP messages.
- Oversized input and invalid numeric ranges.
- Slow clients that do not read responses.
- Disconnect and ownership release.
- Conflicting ownership, priority checks, and preemption.
- Superseded updates and `FLUSH` barriers.
- Hardware failures during multi-write updates.
- Bounded memory during sustained overload.
- Scheduler behavior with short activity pulses.

The main optimization target is avoiding redundant controller writes and stale work—not reducing a readable network command by a few bytes. Actual hardware measurements should determine the final update interval and whether a binary or UDP extension is justified.
