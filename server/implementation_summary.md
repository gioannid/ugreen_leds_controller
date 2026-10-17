# UGREEN LED Server — Stages 1–3 Implementation

## Build & Test

```bash
cd server/
make clean && make     # Builds ugreen_leds_server, test_runner, example_client
make test              # Runs the full test suite (60 tests, all pass)
```

## What was built

### Stage 1: Backend and Parser

| File | Purpose |
|---|---|
| [`include/led_state.h`](include/led_state.h) | `LedId`, `OpMode`, `RgbColor`, `LedState`, `LedPatch` types; parsing/validation helpers |
| [`src/led_state.cpp`](src/led_state.cpp) | Conversion, RGB parsing, 16-bit cycle timing validation |
| [`include/led_backend.h`](include/led_backend.h) | Abstract `LedBackend` interface, `Capabilities`, `HardwareException` |
| [`include/fake_backend.h`](include/fake_backend.h) | `FakeLedBackend` — full in-memory simulator with failure injection and operation history |
| [`include/protocol.h`](include/protocol.h) | Protocol framing limits, `StreamBuffer`, `ProtocolParser`, request/response types |
| [`src/protocol.cpp`](src/protocol.cpp) | Strict typed parser: `HELLO`, `CAPS`, `SET`, `GET`, `RESET`, `FLUSH`; duplicate-field & unknown-field rejection |

### Stage 2: Local Service

| File | Purpose |
|---|---|
| [`include/ownership.h`](include/ownership.h) / [`src/ownership.cpp`](src/ownership.cpp) | Priority-based LED ownership, atomic multi-LED preemption, RESET, disconnect release |
| [`include/scheduler.h`](include/scheduler.h) / [`src/scheduler.cpp`](src/scheduler.cpp) | Desired/applied state tracking, update coalescing, single in-flight batch, FLUSH barriers |
| [`include/hardware_worker.h`](include/hardware_worker.h) / [`src/hardware_worker.cpp`](src/hardware_worker.cpp) | Dedicated worker thread for serialized HW access; eventfd wakeup; bounded queue |
| [`include/reactor.h`](include/reactor.h) / [`src/reactor.cpp`](src/reactor.cpp) | Nonblocking `poll()`-based network reactor; Unix + TCP listeners; session management |
| [`include/logger.h`](include/logger.h) / [`src/logger.cpp`](src/logger.cpp) | Thread-safe leveled logger (DEBUG/INFO/WARN/ERROR) with ISO-8601 UTC timestamps; screen (stdout) or file target; silent until initialized so tests stay quiet |
| [`src/main.cpp`](src/main.cpp) | Entry point with CLI args (`--socket`, `--bind`, `--port`, `--interval`, etc.) |
| [`examples/client.cpp`](examples/client.cpp) | Example client (Unix socket or `--tcp`) |

### Stage 3: Multiple Remote Clients

| File | Purpose |
|---|---|
| [`include/config.h`](include/config.h) / [`src/config.cpp`](src/config.cpp) | INI config parsing: `[server]` + `[access.<name>]` + `[fallback]`; IPv4/IPv6 CIDR sources; most-specific-prefix rule selection; full validation (server refuses to start on any error); dev defaults for unconfigured runs |
| [`include/reactor.h`](include/reactor.h) / [`src/reactor.cpp`](src/reactor.cpp) | Connection authorization at accept time, per-rule connection limits, LED/verb/priority-ceiling enforcement, per-connection command rate limit, server-wide hardware-read rate limit, bounded per-session send queue with overflow disconnect (backpressure) |
| [`src/main.cpp`](src/main.cpp) | `--config` loading (CLI overrides config), single-instance `flock` lock file, graceful SIGINT/SIGTERM shutdown, logging setup (`--log-file`, `--verbose`) and startup logging of transport + access rules |
| [`systemd/ugreen-leds-server.service`](systemd/ugreen-leds-server.service) | Hardened unit: dedicated user, `RuntimeDirectory`, `UMask=0077`, `DeviceAllow` for the I2C bus, no-new-privileges, private tmp/home |
| [`examples/ugreen-leds-server.conf`](examples/ugreen-leds-server.conf) | Example config: local admin over Unix socket, LAN rules with LED subsets / verbs / priority ceilings / per-rule connection caps, fallback state |

### Test Suite (60 tests)

| Test File | Tests |
|---|---|
| [`tests/protocol_tests.cpp`](tests/protocol_tests.cpp) | Fragmented/combined TCP messages, line overflow, all verb parsing, duplicate fields, timing constraints, response serialization |
| [`tests/fake_backend_tests.cpp`](tests/fake_backend_tests.cpp) | Capabilities, apply/read, unsupported LED exception, injected failures |
| [`tests/config_tests.cpp`](tests/config_tests.cpp) | Valid full config, most-specific prefix selection, local-only Unix rule, duplicate network/name rejection, missing source, invalid values, unknown sections/keys, dev defaults, IPv6 parsing, permission helpers, logging keys (`log_file`, `verbose`) |
| [`tests/logger_tests.cpp`](tests/logger_tests.cpp) | Level gating (info default, debug with verbose), screen/file targets, line format and timestamp, append across restarts, unopenable-file failure, pre-init no-op |
| [`tests/ownership_tests.cpp`](tests/ownership_tests.cpp) | Implicit acquisition, priority preemption/rejection, atomic multi-LED, RESET, disconnect release |
| [`tests/scheduler_tests.cpp`](tests/scheduler_tests.cpp) | Update coalescing (20→1 write), no-op suppression, short-flash coalescence, FLUSH barrier success/failure |
| [`tests/integration_tests.cpp`](tests/integration_tests.cpp) | Full end-to-end over Unix socket (HELLO→CAPS→SET→GET→FLUSH→GET→RESET); TCP multi-client priority preemption; `reply=none` suppression |
| [`tests/access_tests.cpp`](tests/access_tests.cpp) | No-match denial, LED restriction, verb restriction, priority ceiling + stronger-rule preemption, per-rule connection limit + release, server-wide connection limit, command rate limit with refill, hardware-read rate limit, slow-client backpressure disconnect |
| [`tests/stress_tests.cpp`](tests/stress_tests.cpp) | 1000-connection churn with `VmRSS` growth check (< 32 MB), 6-thread sustained parallel load, hardware failure storm (100 failing FLUSH rounds then recovery) |

## Architecture Alignment

The implementation follows the ARCHITECTURE.md design:

- **Two threads**: network reactor (poll-based) + hardware worker (condition variable + eventfd wakeup)
- **Protocol v1**: LF-delimited ASCII, 1024-byte line limit, 8 KiB recv / 32 KiB send buffers
- **Ownership**: implicit on mutation, priority-based preemption, atomic multi-LED, RESET/disconnect release with fallback
- **Scheduling**: per-LED desired/applied state, dirty-field tracking, single in-flight batch, generation-based FLUSH barriers
- **`reply=none`**: suppresses `OK ACCEPTED` but not errors
- **Timing validation**: rejects `on_ms + off_ms > 65535` (16-bit cycle overflow) and zero-cycle blink/breath

### Stage 3 behavior

- **Authentication at accept**: peer address (Unix socket ⇒ `local`, TCP ⇒ source IP) is matched against access rules using most-specific prefix; unmatched peers are answered `AUTH_FAILED` and closed before any command is processed.
- **Authorization per command**: LED subset, verb list, and priority ceiling enforced per connection (`FORBIDDEN led=` / `FORBIDDEN verb=` / `REJECTED reason=priority`). The configured `priority` is a ceiling: weaker (higher-numbered) priorities are allowed, stronger ones are rejected.
- **Limits**: per-rule and server-wide connection caps (`AUTH_FAILED reason=connection_limit|server_limit`), per-connection token-bucket command rate limit (burst = rate), server-wide token bucket for `GET source=hardware` (`RATE_LIMITED`). Rate 0 disables the limit.
- **Backpressure**: each session has a bounded send queue (`kMaxQueuedOutput` = 32 KiB). The reactor keeps reading input (bounded by the 8 KiB recv buffer and the TCP window) and registers `POLLOUT` only while the queue is non-empty so responses drain as the client reads. A client that never consumes output grows its queue until overflow, at which point the server drops the connection; the service stays responsive to other clients. Registering `POLLOUT` unconditionally would make `poll()` return immediately for every idle-but-writable client, starving the scheduler's time-based ticks and busy-spinning the reactor.
- **Stale hardware errors**: a failed batch records `error_generation`; FLUSH barriers are failed only by errors at or below the current desired generation, so a retry in flight does not poison later barriers.
- **Configuration**: `[server]` transport/limit settings, `[access.<name>]` rules (source, leds, verbs, priority, max_connections), `[fallback]` per-LED state. Without config (or with no rules) the server applies development defaults: local Unix + loopback access to all LEDs at priority 0 — documented as unsafe for production.
- **Logging**: leveled, thread-safe logging for production observability and troubleshooting. Info-level and above (startup/shutdown, listener bind results, connections accepted/rejected, authentication, authorization violations, hardware failures) are on by default and go to the screen (stdout, captured by the journal); `--verbose` / `verbose=true` adds debug-level detail (per-command processing, rate-limiting, scheduler batches, hardware read/write results, protocol errors with the offending line). `--log-file` / `log_file=` redirects the log to a file (append mode, per-line flush); an unopenable file aborts startup. Line format: `2026-10-17T10:15:30.123Z [INFO] message`.
- **Deployment**: systemd unit with a dedicated user, runtime directory, socket file mode 0700, device access to the I2C bus, and standard hardening; `--lock-file` enforces a single instance.

### Test notes

- Network tests allocate loopback ports dynamically (`bind` to port 0) so they cannot collide with unrelated processes on the host.
- The backpressure test pins the client's `SO_RCVBUF` (disables rcvbuf auto-tuning) and floods more GETs than the kernel can absorb while never reading, forcing the send-queue overflow; teardown is detected via EOF or RST.
- Memory growth is checked via `VmRSS` from `/proc/self/status` (valgrind is not available in the build environment).
