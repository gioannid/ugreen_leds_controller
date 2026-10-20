# UGREEN LED Server — Stages 1–4 Implementation

## Build & Test

```bash
cd server/
make clean && make     # Builds ugreen_leds_server, test_runner, example_client, hw_smoke_test, hw_monkey_test
make test              # Runs the full test suite (91 tests, all pass)
```

`hw_smoke_test` and `hw_monkey_test` are separate, statically linked binaries
for the production machine with the real LED hardware (see Stage 4 below).
`hw_monkey_test` needs a *running* server with `backend=i2c` and drives one
LED through many random transitions, verifying every step with
`GET source=hardware`.

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

### Stage 4: Integrate with existing controller code for real LED HW

| File | Purpose |
|---|---|
| [`include/i2c_led_backend.h`](include/i2c_led_backend.h) / [`src/i2c_led_backend.cpp`](src/i2c_led_backend.cpp) | `I2cLedBackend` — wraps the existing userspace controller (`cli/ugreen_leds.cpp`, compiled in unmodified, no copies); SMBus I801 adapter auto-discovery (the CLI's own rule); `plan_led_update()` maps a partial patch to the minimal controller command sequence (pure function, transition semantics from `resolve_target_state()`); CLI-style retry timing; per-command verification via the chip's last-modification result register (0x80) plus a post-apply readback that the resulting state as a whole matches the plan |
| [`include/config.h`](include/config.h) / [`src/config.cpp`](src/config.cpp) | `[server]` key `backend` (fake\|i2c); validation |
| [`src/main.cpp`](src/main.cpp) | `--backend` flag; constructs the selected backend; refuses to start with a clear error when the I2C controller is unavailable or reports zero LEDs; warns when the fake simulator is in use |
| [`tests/hw/hardware_smoke_test.cpp`](tests/hw/hardware_smoke_test.cpp) | `hw_smoke_test` — standalone real-hardware smoke test (see below) |
| [`tests/hw/hardware_monkey_test.cpp`](tests/hw/hardware_monkey_test.cpp) | `hw_monkey_test` — standalone real-hardware resilience test: random transition fuzzing against a live server (see below) |

**Backend behavior** (`I2cLedBackend`):

- Constructor resolves the device (first "SMBus I801 adapter" in `/sys/class/i2c-dev`, the CLI's own rule — kept server-side only to name the exact device in error messages), opens it through the unmodified `ugreen_leds_t::start()` at the chip's fixed address `0x3a`, and probes all 10 LED IDs to build the capability report. Throws `HardwareException` with the `errno` message when the adapter, device node, or chip is missing — the server refuses to start.
- `apply()` reads the current hardware state first, merges the patch (which is a *diff* from the scheduler), and issues only the changed controller commands: `set_rgb`, `set_brightness`, `set_onoff`, `set_blink`/`set_breath` (blink/breath commands carry the cycle timing, so a timing change in the same mode re-issues exactly one mode command).
- Every command is verified against the chip's result register with the CLI's proven retry timing (settle delays, 5 attempts); a write the chip did not acknowledge is reported as `HardwareException` and handled by the existing scheduler error/retry path.
- `read()` maps a verified 11-byte status block to `LedState` (timing zeroed for non-blink modes, where the chip may retain stale values).

**Mode transition hardening** (follow-up to the blink/breath corrections):

The blink/breath fixes exposed a generic weakness: mode/timing semantics were
implemented independently in three places (scheduler `apply_patch`,
`apply_fallback`, and the I2C planner), the scheduler recorded the *planned*
target state as "applied" instead of what the hardware actually reached, and
no transition was ever verified as a whole. The hardening:

- **Single source of truth** — `resolve_target_state()` /
  `normalize_led_state()` in `led_state.cpp` encode the whole contract:
  timing without a mode implies blink; entering blink/breath from a different
  mode without explicit timing uses the mode's default cycle (500/500,
  1000/1000) rather than a stale inherited one; a zero cycle falls back to
  the default; off/on states carry no active cycle. Scheduler, fake backend
  and I2C planner all resolve through it, so they can no longer disagree.
- **Applied state is device truth** — `LedBackend::apply()` returns the
  post-apply state; the hardware worker records that as the scheduler's
  applied state. Any drift (external writers, power cycle) is detected on the
  next batch and re-issued, and a FLUSH barrier only completes once the
  hardware genuinely reached the desired state.
- **Post-apply verification** — after every command is acknowledged, the
  I2C backend reads the LED back and confirms the state as a whole matches
  the plan (mode, color, brightness, and the active cycle; in off/on modes
  zero color/brightness targets are not verifiable since the chip ignores
  them, non-zero targets must converge; a mismatch is a `HardwareException`
  into the existing error/retry path).
- **Zero color/brightness policy** — the chip acknowledges but ignores
  zero-valued color/brightness updates in off/on modes (verified on real
  hardware: `brightness=0` and `color=0,0,0` are treated as "keep current
  value"; a default `off` fallback of `rgb=0,0,0 brightness=0` can never
  converge and would retry forever, flooding the log). The scheduler
  therefore does not chase a zero color/brightness target in off/on modes —
  the value is re-programmed together with the next mode change to
  blink/breath, which the chip does apply.
- **Startup calibration** — the reactor reads every supported LED at start
  and adopts the real state as the desired/applied baseline, so the first
  client update is diffed against the truth (previously a first
  `SET mode=off` while the firmware had the LED lit was a silent no-op).
- **State-dependent cycle validation** — the parser can only check an
  `on_ms`/`off_ms` pair within one SET; the reactor now also rejects a
  single-field update whose resolved cycle would overflow the chip's 16-bit
  cycle, or resolve to a zero half-cycle in blink/breath — the chip rejects
  such commands (`INVALID_ARGUMENT field=on_ms`).

**Real-hardware smoke test** (`hw_smoke_test`):

- Built as a separate statically linked binary (`make hw_smoke_test`) so it can be copied to the production machine as a single file and run there, e.g. `sudo ./hw_smoke_test --confirm`.
- Reports the detected controller and supported LEDs, then drives one LED (default: first available disk LED; the power LED is refused without `--include-power`) through: solid red → blink green (250/250 ms) → breath blue (500/500 ms) → solid white → off. Every step is verified by reading the controller back (mode, RGB, brightness, and blink/breath timing must match exactly).
- With `--confirm` the test additionally asks the user to confirm each visible state by eye (and for an initial "proceed?" approval); a "no" answer is counted as a failed check.
- Verifies that a non-present LED ID is rejected by the backend, and restores the LED's original state at the end. Exit codes: 0 = passed, 1 = checks failed, 2 = environment error, 3 = cancelled by user.

**Real-hardware monkey test** (`hw_monkey_test`):

- Built as a separate statically linked binary (`make hw_monkey_test`); like `example_client` it is a pure protocol client (no server objects), so it is a single file to copy to the production machine. It needs a **running** `ugreen_leds_server` with `backend=i2c`: `sudo ./hw_monkey_test --socket /tmp/ugreen_leds.sock --confirm`.
- Drives one LED (default: first available disk LED; same selection rules as the smoke test) through `--iterations` (default 200) random partial updates — every combination of mode / rgb / brightness / on_ms / off_ms with edge values (5 ms … 60 s halves, 16-bit boundary), plus ~5% deliberately invalid patches (zero cycle, cycle overflow, single-field overflow) that the server must reject without disturbing the state.
- After every accepted update it waits for a `FLUSH` barrier, then verifies with `GET source=hardware` (retried across the server-wide read rate limit): mode, RGB and brightness must match, and blink/breath cycles must match exactly (off/on readbacks may retain stale cycle registers, so timing is compared only for active animation). The server's `GET source=applied` must agree with the hardware truth; `GET source=desired` must agree with the logical expected state (the two diverge by design only when a zero color/brightness target is set in off/on mode — the chip retains the previous value, which the test's independent model also predicts).
- The expected-state model is an *independent* re-implementation of the transition contract (not shared server code), so a server regression fails the test rather than mirroring it. The LED's original state is restored at the end. `--seed` makes runs reproducible. Exit codes: 0 = passed, 1 = checks failed, 2 = environment error, 3 = cancelled by user.

**Stage 4 tests without hardware** (in `test_runner`, follow the existing paradigm):

- `backend` config key: parsing, rejection of unknown backends, defaults.
- Device-discovery sanity and an environment-aware construction test: on a host without the UGREEN chip the backend must fail cleanly; on the production host it must come up, report LEDs, and reject non-present ones. The same test suite therefore passes on both machines.

### Test Suite (79 tests)

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
| [`tests/i2c_backend_tests.cpp`](tests/i2c_backend_tests.cpp) | Stage 4 without hardware: `plan_led_update()` command planning, controller-op descriptions, device discovery sanity, environment-aware backend construction, `backend` config parsing and rejection cases |

## Architecture Alignment

The implementation follows the ARCHITECTURE.md design:

- **Two threads**: network reactor (poll-based) + hardware worker (condition variable + eventfd wakeup)
- **Protocol v1**: LF-delimited ASCII, 1024-byte line limit, 8 KiB recv / 32 KiB send buffers
- **Ownership**: implicit on mutation, priority-based preemption, atomic multi-LED, RESET/disconnect release with fallback
- **Scheduling**: per-LED desired/applied state, dirty-field tracking, single in-flight batch, generation-based FLUSH barriers
- **`reply=none`**: suppresses `OK ACCEPTED` but not errors
- **Timing validation**: rejects `on_ms + off_ms > 65535` (16-bit cycle overflow), zero-cycle blink/breath, and zero half-cycles (the chip rejects blink/breath with `on_ms=0` or `off_ms=0`)
- **Single copy of the controller code, unmodified**: the server compiles `cli/i2c.cpp` and `cli/ugreen_leds.cpp` directly (objects `cli_i2c.o`, `cli_ugreen_leds.o`); `I2cLedBackend` wraps `ugreen_leds_t` as-is and keeps no copies of the wire protocol. The `cli/` sources are untouched, so the legacy CLI carries zero regression risk from the server.
- **Two hardware paths, one interface**: `FakeLedBackend` (default, development) and `I2cLedBackend` (real HW, `backend=i2c`) both satisfy the `LedBackend` interface; the reactor, scheduler, ownership, and worker code is backend-agnostic.

### Stage 3 behavior

- **Authentication at accept**: peer address (Unix socket ⇒ `local`, TCP ⇒ source IP) is matched against access rules using most-specific prefix; unmatched peers are answered `AUTH_FAILED` and closed before any command is processed.
- **Authorization per command**: LED subset, verb list, and priority ceiling enforced per connection (`FORBIDDEN led=` / `FORBIDDEN verb=` / `REJECTED reason=priority`). The configured `priority` is a ceiling: weaker (higher-numbered) priorities are allowed, stronger ones are rejected.
- **Limits**: per-rule and server-wide connection caps (`AUTH_FAILED reason=connection_limit|server_limit`), per-connection token-bucket command rate limit (burst = rate), server-wide token bucket for `GET source=hardware` (`RATE_LIMITED`). Rate 0 disables the limit.
- **Backpressure**: each session has a bounded send queue (`kMaxQueuedOutput` = 32 KiB). The reactor keeps reading input (bounded by the 8 KiB recv buffer and the TCP window) and registers `POLLOUT` only while the queue is non-empty so responses drain as the client reads. A client that never consumes output grows its queue until overflow, at which point the server drops the connection; the service stays responsive to other clients. Registering `POLLOUT` unconditionally would make `poll()` return immediately for every idle-but-writable client, starving the scheduler's time-based ticks and busy-spinning the reactor.
- **Stale hardware errors**: a failed batch records `error_generation`; FLUSH barriers are failed only by errors at or below the current desired generation, so a retry in flight does not poison later barriers.
- **Configuration**: `[server]` transport/limit settings, `[access.<name>]` rules (source, leds, verbs, priority, max_connections), `[fallback]` per-LED state. Without config (or with no rules) the server applies development defaults: local Unix + loopback access to all LEDs at priority 0 — documented as unsafe for production.
- **Logging**: leveled, thread-safe logging for production observability and troubleshooting. Info-level and above (startup/shutdown, listener bind results, connections accepted/rejected, authentication, authorization violations, hardware failures) are on by default and go to the screen (stdout, captured by the journal); `--verbose` / `verbose=true` adds debug-level detail (per-command processing, rate-limiting, scheduler batches, hardware read/write results, protocol errors with the offending line). `--log-file` / `log_file=` redirects the log to a file (append mode, per-line flush); an unopenable file aborts startup. Line format: `2026-10-17T10:15:30.123Z [INFO] message`.
- **Deployment**: systemd unit with a dedicated user, runtime directory, socket file mode 0700, device access to the I2C bus, and standard hardening; `--lock-file` enforces a single instance.

### Stage 4 behavior

- **Backend selection**: `backend=fake` (default) keeps the development behavior (simulator, real hardware untouched, startup warning); `backend=i2c` opens the real controller and the server refuses to start when the controller cannot be opened or reports zero LEDs.
- **Fail-fast with diagnostics**: missing adapter, missing device node, and chip-not-present all produce a distinct `HardwareException` message (with the `errno` text) that is logged at startup.
- **Ownership of I/O**: all controller reads/writes happen on the hardware worker thread; `capabilities()` returns the constructor-time probe cache, so the reactor can serve CAPS without touching the bus.
- **Verification semantics**: a batch is "applied" only when every command was acknowledged by the chip's result register; otherwise the existing error_generation/retry machinery applies.

### Test notes

- Network tests allocate loopback ports dynamically (`bind` to port 0) so they cannot collide with unrelated processes on the host.
- The backpressure test pins the client's `SO_RCVBUF` (disables rcvbuf auto-tuning) and floods more GETs than the kernel can absorb while never reading, forcing the send-queue overflow; teardown is detected via EOF or RST.
- Memory growth is checked via `VmRSS` from `/proc/self/status` (valgrind is not available in the build environment).
- The Stage 4 construction test is environment-aware: it probes whether a UGREEN controller is actually reachable at the chip's fixed address (0x3a) and asserts either a clean `HardwareException` (development host) or a working backend (production host), so `make test` passes on both.
- Real-hardware verification lives in the separate `hw_smoke_test` binary (statically linked, single file) — it is not part of `make test` on purpose, because it changes the physical LED state and only runs where the hardware exists.
