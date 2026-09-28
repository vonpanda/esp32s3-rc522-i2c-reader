# Firmware Architecture — ESP32-S3 + HS-S62A-PL RFID Reader

**English** ｜ [中文](ARCHITECTURE.zh-CN.md)

> A production-firmware skeleton built for "high real-time + high stability" requirements.
> Related: [BRINGUP.md](BRINGUP.md) power-up troubleshooting handbook ｜ [../lib/YFROBOTRFIDI2C/PATCHES.md](../lib/YFROBOTRFIDI2C/PATCHES.md) library patch log

> **Note on log samples**: the firmware prints Chinese. Log lines are reproduced
> verbatim (they are what you will actually see on the serial port); English
> glosses appear in the surrounding text.

---

## 1. Design goals and hard constraints

| Item | Value | Source |
|---|---|---|
| MCU | ESP32-S3 dual-core Xtensa LX7 @ 240 MHz | board definition |
| Memory | 320 KB SRAM + 2 MB PSRAM | `rymcu-esp32-s3-devkitc-1.json` |
| Flash | 8 MB, partition table `default_8MB.csv` | same |
| Peripheral | HS-S62A-PL (MFRC522 / SI522A), I2C, default 0x28 | module user manual V1.1 |
| Module supply | 3.3–5 V, sensing range 0–2 cm | same |

**Latency target**: card presented → event delivered to the business layer in ≤ 50 ms (P99).
**Stability target**: 72 h continuous operation with zero watchdog resets, zero missed reads, zero hangs.

Those two targets decide every trade-off below: **no call that can block unboundedly is allowed on a task path.**

---

## 2. Layered architecture

Dependencies point strictly downward; no layer knows the one above it exists.

```
┌──────────────────────────────────────────────────────────────┐
│ Application   src/main.cpp                                   │
│  Task orchestration · card state machine · access decision ·   │
│  queue · watchdog · boot-loop protection                      │
└───────────────┬──────────────────────────────────────────────┘
                │ calls               ▲ CardEvent (queue, by value)
┌───────────────▼──────────────────────┴───────────────────────┐
│ Device        src/rfid_reader.{h,cpp}                        │
│  mount / poll / heal · chip liveness (VersionReg) ·           │
│  presence detection (WUPA + HaltA)                            │
└───────────────┬──────────────────────────────────────────────┘
                │ calls
┌───────────────▼──────────────────────────────────────────────┐
│ Bus           src/i2c_bus.{h,cpp}                            │
│  init · error classification · line recovery (9 clocks+STOP) · │
│  counters                                                     │
└───────────────┬──────────────────────────────────────────────┘
                │
┌───────────────▼──────────────────────────────────────────────┐
│ Chip driver   lib/YFROBOTRFIDI2C  (patched: bounded timeouts) │
└──────────────────────────────────────────────────────────────┘
```

Cross-cutting: `app_config.h` (every tunable), `app_log.{h,cpp}` (the only serial-port exit, mutex-protected).

### Layer responsibilities and prohibitions

| Layer | Responsibility | Explicitly forbidden |
|---|---|---|
| Bus | Move bytes, give error codes meaning, recover the bus | Must not know device semantics (must not know 0x37 is a version register) |
| Device | Device semantics (mount / poll / presence / heal) | Must not create tasks, must not make IO decisions |
| Application | Tasks, queue, business decisions | Must not touch `Wire` directly |

---

## 3. Task layout

```
Core 1 (APP CPU)                          Core 0 (PRO CPU)
├─ rfidTask   prio 6  stack 4096          └─ reportTask prio 3  stack 6144
│   sole owner of I2C                            business / reporting / actuators
└─ healthTask prio 2  stack 4096
    reads counters only, never touches I2C

  loopTask   prio 1  (Arduino host task; kept, does a 1 s idle sweep)
```

| Task | Core | Prio | Stack | Period | Max single block | Responsibility |
|---|---|---|---|---|---|---|
| `rfidTask` | 1 | 6 | 4096 B | 30 ms | ≈15 ms | poll, debounce, post events, link recovery |
| `reportTask` | 0 | 3 | 6144 B | event-driven | unlimited (only here) | all outbound actions: serial / MQTT / relay |
| `healthTask` | 1 | 2 | 4096 B | 1000 ms | < 1 ms | health, stack and heap watermarks |
| `loopTask` | 1 | 1 | 8192 B | 1000 ms | 1 ms | reserved for OTA / buttons / LED |

### Why the cores are split this way

- **Acquisition owns Core 1**: the Wi-Fi/BLE stack is pinned to Core 0 at priority 18–23. Putting I2C acquisition on Core 0 lets the stack's long critical sections interleave with it, making worst-case jitter unpredictable. After the split, acquisition jitter is only affected by `healthTask` on the same core, which has lower priority.
- **`reportTask` owns Core 0**: all outbound IO (MQTT/HTTP later) can block for hundreds of milliseconds; on Core 1 it would directly disturb the acquisition cadence.
- **Only three priority levels**: more levels invite priority inversion and starvation. Here it only needs to guarantee acquisition > business > housekeeping.

---

## 4. Concurrency and IPC

### 4.1 The single-writer rule (core)

**Only `rfidTask` calls I2C in the entire process.** Three direct consequences:

1. **No locks on the hot path** — and with no locks there is no priority inversion and no deadlock.
2. `Wire` is not thread-safe (and the vendor library does not protect it either); a single writer makes that hazard vanish.
3. Acquisition timing is fully deterministic and unaffected by other tasks' scheduling.

When `healthTask` needs bus information it only reads `i2cbus::diag()` (pure counters) — it does not issue I2C.

### 4.2 Event queue

| Item | Value |
|---|---|
| Type | `CardEvent` (fixed-size POD: UID / timestamp / dwell time, ~32 B) |
| Depth | 8 (`CFG_Q_CARD_LEN`) |
| Posting | `xQueueSend(..., 0)` — non-blocking |
| When full | **Drop and count**, never block the acquisition task |

> Trade-off: acquisition's real-time behaviour outranks "never lose an event". In an
> access-control scenario card events are re-entrant — a card still on the reader is
> read again and again, so dropping one does not affect correctness, whereas blocking
> the acquisition task for 30 ms would break the entire cadence. If you later move to
> a billing scenario where not a single event may be lost, lengthen the queue and
> persist to NVS — **do not** switch to blocking delivery.

### 4.3 Logging

`appLogLine()` carries a built-in mutex (with priority inheritance), so formatting plus
write-out is atomic. Concurrent `printf` through Arduino-ESP32's `Serial` (HWCDC) would
clobber each other's buffer and produce interleaved lines, so no task may call `Serial`
directly.

#### Dual-channel output

On ESP32-S3-DevKitC-1 with `ARDUINO_USB_CDC_ON_BOOT=1` + `ARDUINO_USB_MODE=1`, the
framework's two objects map as follows (verified against framework sources
`HWCDC.h:110-116`, `HardwareSerial.h:329-333`):

| Object | Type | Physical port |
|---|---|---|
| `Serial` | `HWCDC` | native USB port (`/dev/cu.usbmodem*`) |
| `Serial0` | `HardwareSerial` | **UART0 → GPIO43/44 → on-board USB-UART bridge** |

`appLogInit()` opens both channels according to `CFG_LOG_USB_CDC` / `CFG_LOG_UART0`,
with identical content. The reasoning is practical: **the thing you fear most on site is
"the firmware is printing to a port your colleague didn't plug in"** — and that
practically always happens when handing a board over (the other person may only have a
single USB-TTL adapter on hand). The cost is +0.7 % flash, traded for "whoever plugs into
either port can see it".

The two channels are independent and never block each other — if one cannot accept a
write, it does not hold up the other:

- **USB CDC**: `setTxTimeoutMs()` is mandatory. `HWCDC::write()` waits synchronously for
  the host when the host is absent, and `tx_timeout_ms` is its only bound (the
  `tries = tx_timeout_ms` loop in the source). Without it, one log line after unplugging
  the USB cable blocks the calling task outright — the classic USB CDC trap. This project
  uses 20 ms.
- **UART0**: send buffer raised to 512 B (`setTxBufferSize()`, which **only takes effect
  if called before the first `begin()`**). At 115200 a ~200 B log line fits in the buffer,
  so writes essentially never block.

#### Output levels (designed for "a colleague with the board in hand")

| Level | Content | Cadence |
|---|---|---|
| `SELF` | boot hardware self-check verdict (pin four-state + solder bridge) | once at boot |
| `CARD` | card events, with `>>>` / `<<<` as visual anchors | event-driven |
| `STATE` | one status line: uptime / in-out-deny-drop counters / link state | `CFG_STATE_LINE_MS` (10 s) |
| `WARN` | anomalies: stack watermark, heap, queue overflow | only when a threshold is crossed |
| `HEALTH` | full diagnostics (stack watermark, heap, bus histogram) | only when `CFG_VERBOSE_HEALTH=1` |

**The intent is to stay quiet while healthy.** That way "a `WARN` line appeared in the
log" is itself a meaningful signal, rather than being diluted by three lines per second
of routine output. During development set `CFG_VERBOSE_HEALTH` to 1 to get full
diagnostics back.

> ⚠️ There is a coupling trap worth remembering here: `healthTask`'s **loop period**
> doubles as the watchdog feed, while the **status-line period** is only logging. An
> earlier version shared `CFG_HEALTH_PERIOD_MS` between them, so raising it to 10 s made
> the task get reset by the 5 s watchdog before it ever printed. They are now two
> independent constants: `CFG_HEALTH_TICK_MS` (feeds the dog; must stay far below
> `CFG_TWDT_SECONDS`) and `CFG_STATE_LINE_MS` (printing; any value works).

---

## 5. Timing budget (100 kHz I2C)

One byte on the wire ≈ 9 bit ÷ 100 kHz ≈ 90 µs; a register write (address + register +
data) ≈ 3 B ≈ 270 µs; a register read (address write then read back) is two transactions
≈ 600 µs.

| Operation | Composition | Time |
|---|---|---|
| Address probe `probe()` | 1 byte | ≈ 0.1 ms |
| `ReadVersion()` (link liveness) | read 1 register | ≈ 0.6 ms |
| `WUPA` (wake-up) | ~6 writes + 1–3 reads | ≈ 2–3 ms |
| `SELECT` (anticollision + select) | includes CRC computation, ~10 register accesses | ≈ 4–6 ms |
| `HaltA` | 2 writes + 1 read | ≈ 1 ms |
| **One poll with a card present** | | **≈ 8–10 ms** |
| **One poll with no card** | card sits on the RC522's internal timer (`CFG_RC522_TRELOAD`=512 → 12.8 ms) | **≈ 12.8 ms** |

**Card detection latency** = remaining poll wait (0–30 ms) + one poll (≈10 ms)
→ **P50 ≈ 20 ms, P99 ≈ 45 ms**, meeting the ≤50 ms target.

> Note: **"no card" is slower than "card present".** With no card the code waits for the
> RC522's own timer to expire, and that time is pure I2C idling. That is exactly why
> `TReloadReg` was changed from the 25 ms default to 12.8 ms. If you see occasional
> missed weak cards in the field, setting `CFG_RC522_TRELOAD` back to 1000 recovers a
> significant amount of sensitivity, at the cost of stretching the acquisition period
> from 30 ms to 45 ms.

**Need it faster?** Set `CFG_RFID_POLL_MS` to 15, `CFG_RC522_TRELOAD` to 256 (≈6.4 ms),
and raise `CFG_I2C_FREQ_HZ` to 400000. A poll then costs ≈3 ms and P99 detection latency
is ≈20 ms — provided the bus has 4.7 kΩ external pull-ups (see BRINGUP.md).

---

## 6. Failure model and countermeasures

| # | Failure | Field symptom | How this architecture handles it | Code |
|---|---|---|---|---|
| 1 | Bad power-up sequencing (a known issue documented in the vendor library's own README) | no response at all after power-up; only a power cycle recovers it | bounded init (100 ms max) → liveness check fails → automatic heal/re-mount, no infinite loop | `rfid::begin/heal`, library PATCH-1 |
| 2 | Slave reset mid-transfer leaves SDA held low permanently | every transaction returns `endTransmission()==5` | line recovery: 9 clocks + STOP to kick the slave out of the deadlock | `i2cbus::recover` |
| 3 | Module drops off / ribbon cable works loose | reads suddenly all fail | link probe every poll (cheap address check + periodic deep `VersionReg` check); only 5 consecutive failures declare it offline and trigger heal | `rfid::poll`, `rfid::linkState` |
| 4 | Task hangs (library infinite loop / hardware wait) | device looks alive but stops reading and never reboots | task watchdog (TWDT) with `panic=true`: on timeout it prints a backtrace and resets | `main.cpp` |
| 5 | Crash on boot → reset storm | serial repeatedly prints the boot banner | boot counter in the RTC domain; past the threshold it enters **safe mode** (self-check only, no business tasks) so the serial port stays diagnosable | `countBootAndLog` |
| 6 | Supply sag (current inrush when the antenna starts) | random reboots | banner prints `reset_reason`, so `BROWNOUT` is immediately identifiable; the design reserves a `VCC` load switch | `resetReasonName` |
| 7 | Event queue overflow | business layer loses events | count and drop; the acquisition side never blocks | `rfidTask` |
| 8 | Heap fragmentation (the heal path mallocs/frees) | allocation failures after days of uptime | housekeeping task continuously reports `getMinFreeHeap()`; a falling minimum is the early warning | `healthTask` |
| 9 | Spurious reads (card hovering at the reader's edge) | one tap reported as several | a card must be unanswered for 500 ms continuously to count as removed | `CFG_CARD_RELEASE_MS` |
| 10 | **Reader absent for a long time** (wiring unfinished / module not fitted) | log flooded with "heal failed"; bus hammered every 30 ms | while the link is unavailable, exponential backoff (250 ms → 10 s cap), same-message log throttled to 5 s, state transitions printed immediately | `rfidTask` path A |
| 11 | **`SDA`/`SCL` swapped** | levels, pull-ups and waveform all normal, error codes uniformly NACK, nothing on the whole bus answers — very easily misread as "the module is dead" | bring-up firmware step 3 auto-retries with the pins swapped; `diagnose()` lists "swapped" as the first suspect for an all-NACK histogram | `bringup/main.cpp`, `i2cbus::diagnose` |

### 6.1 Link tri-state and backoff recovery

The acquisition task has two **mutually exclusive** paths, driven by `rfid::linkState()`:

| State | Meaning | Recovery action | Cost bound |
|---|---|---|---|
| `UP` | mounted and link healthy | none; normal 30 ms polling | — |
| `LOST` | was mounted, now offline (address known) | `heal()`: 9-clock line unstick + full re-init | 3 rounds × (recover + attach + 100 ms) |
| `DOWN` | never mounted (address unknown) | `retry()`: try only 0x28 / 0x2F, **no full-bus scan** | ≤2 I2C transactions |

Both paths **share one exponential backoff curve**: 250 → 500 → 1000 → 2000 → 4000 → 8000
→ 10000 ms (capped). Success resets it to zero; each failure steps it up. During the
backoff wait the task sleeps in `CFG_BACKOFF_TICK_MS` (50 ms) slices, so the upper bound
on noticing link recovery is 50 ms — and the watchdog is fed every 50 ms.

**Why `DOWN` must use the cheap retry rather than reusing `begin()`**: `begin()` includes
a full-bus scan (112 addresses). With line timeouts, a single retry would eat more than
2 seconds and the backoff curve would become meaningless. `retry()` costs a constant
2 transactions, which is what makes "start backing off at 250 ms" possible at all.

**Why back off at all**: an absent reader is the *normal* state of "wiring not finished
yet", not a fault. Hammering the bus every 30 ms yields no new information and drowns the
one log line that matters — which is exactly how the field symptom "continuous flooding +
repeated heal failures" arises.

### 6.2 A diagnostic function must only look at "this probe"

`i2cbus::diagnose()` has the signature `diagnose(const ErrTally &t)` — a **pure function**
that only consumes "this probe's error-code histogram" and never reads cumulative
counters. This rule came out of a real bug:

> Bring-up firmware step 3 (the pin-swap test) scanned with **a different pin
> configuration** and found a device, so the global `ok` counter became 1. From then on
> every "nobody ever answered" predicate (`tx>0 && ok==0`) was dead, and `diagnose()`
> concluded "bus communication is normal" in the face of 451 NACKs — while the operator
> was looking at the exact opposite.

**Conclusion**: counters aggregated across states cannot be used for diagnosis. Diagnosis
must be a pure function of "this moment's evidence", which makes this class of bug
structurally impossible.

### 6.3 On malloc in the recovery path

`Wire.end()` frees two 128 B buffers and `Wire.begin()` mallocs them back (measured
behaviour of the framework's `Wire.cpp`). So each heal does 2 mallocs + 2 frees.

- The trigger is narrow: **5 consecutive link-liveness failures**, never reached in
  normal operation.
- The risk is closed-loop: `healthTask` reports the heap minimum every second, so
  fragmentation shows up well before it becomes a failure.
- If a production line needs 7×24 with zero heap allocation, switch to **hard-resetting
  the module's VCC with a load switch** (see 6.4) — then not even bus recovery is needed.

### 6.4 Two strong hardware recommendations

1. **Put a MOS load switch on the module's VCC** (e.g. AO3401 driven from a GPIO). This is
   the only way to 100 % cure "bad power-up sequencing": when a software reset cannot
   save it, the firmware pulls the rail low for 200 ms and re-powers — equivalent to
   unplugging and replugging the power.
2. **Add 4.7 kΩ pull-ups from SDA/SCL to 3.3 V.** RC522 modules normally ship without
   external pull-ups, relying only on the ESP32's internal ~45 kΩ. At 100 kHz the edges
   are already marginal, and a long ribbon cable produces "works sometimes" behaviour.
   The added resistors do not need removing — they only make the waveform cleaner.

---

## 7. Measured resource usage

| Item | Production firmware | Bring-up firmware | Headroom |
|---|---|---|---|
| RAM | 20 012 B / 327 680 B (6.1 %) | 19 980 B / 327 680 B (6.1 %) | ample |
| Flash | 316 125 B / 3 342 336 B (9.5 %) | 316 721 B / 3 342 336 B (9.5 %) | ample |

(Measured on a full rebuild, 2026-09-28. This project's own code compiles warning-free;
only the vendor library `YFROBOTRFIDI2C.cpp` retains 4
`-Wunused-variable` / `-Wmaybe-uninitialized` warnings — see
`lib/YFROBOTRFIDI2C/PATCHES.md`. Flash grew from 8.8 % to 9.5 % because of the
dual-channel logging, which drives HWCDC and UART0 simultaneously; that is expected.)

Stack watermarks are printed by `healthTask` every second
(`uxTaskGetStackHighWaterMark`). **Acceptance criterion: every task's watermark ≥ 1024 B.**
Below that, raise the corresponding `CFG_TASK_STACK_*`.

---

## 8. Acceptance test checklist

Work through this before going to production; every item needs a record.

| Item | Method | Pass criterion |
|---|---|---|
| Cold-boot mount | power-cycle 20 times, watching the boot log each time | 20/20 mount on the first attempt, no heal |
| Continuous reading | same card held against the reader for 72 h | no reboots (`reset_reason` is POWERON only), no missed reports |
| Rapid taps | 200 consecutive manual taps | reported count = tap count, no duplicates, no omissions |
| Unplug/replug ribbon | with power on, pull the module's SDA, reconnect after 30 s | backoff caps at 10 s + 50 ms sleep slices, so reading resumes within ≤11 s (log shows link recovered) |
| Ribbon left disconnected | pull the module's SDA and **do not** reconnect; observe for 10 min | log converges under backoff to one line per 10 s; bus transactions stop growing (`tx` rises only with retries, not 33/s) |
| Bus held dead | disconnect and re-power the module's VCC | heal runs automatically and recovers, no reset |
| Watchdog effectiveness | temporarily insert `while(1);` into `rfidTask` | TASK_WDT reset within ≤5 s (the 50 ms backoff slices do not starve the WDT) |
| Boot-loop protection | temporarily force a reset at the top of `setup()` | the 8th boot enters safe mode and stops resetting |
| Weak-card boundary | card at the 2 cm edge | record the actual detection distance and compare with the hardware spec |
| **Swapped-pin regression** | deliberately swap SDA/SCL and run the bring-up firmware | step 3 must report "found a device after swapping"; steps 4/5 must still mount using the swapped pins; the conclusion block must state the correct wiring |

---

## 9. Known gaps and roadmap

| Gap | Impact | Suggestion |
|---|---|---|
| No hard module power reset | a rare power-up anomaly needs a manual unplug | add the load switch (6.4) |
| No NVS persistence | changing the whitelist requires a reflash | wire up NVS or a backend push; `isAllowed()` is the only place to change |
| No OTA | field upgrades require opening the enclosure | `loop()` already has a mount point; use dual partitions with rollback |
| Events not persisted before power loss | loss-of-power scenarios lose events | queue-overflow counting is already in place; just add NVS |
| Library still silently drops `PCD_WriteRegister` errors | write failures are silent | already backstopped by `VersionReg` liveness checks; re-verify against PATCHES.md when upgrading |
| Single reader | no redundancy | for multiple readers use `Wire1` (the second I2C controller) plus a second acquisition task — the architecture does not need to change |

---

## 10. File list

```
platformio.ini                     two build environments + pinned versions
src/
  app_config.h                     every tunable (the only file to edit in the field)
  app_log.h/.cpp                   the only serial exit: dual channel + mutex
  i2c_bus.h/.cpp                   bus layer (pin four-state check / solder-bridge test / diagnosis)
  rfid_reader.h/.cpp               device layer
  main.cpp                         application layer (production firmware)
  bringup/main.cpp                 bring-up firmware (no business logic)
lib/YFROBOTRFIDI2C/                vendored driver (patched)
  src/                             driver sources
  PATCHES.md                       ★ mandatory checklist when upgrading the library
docs/vendor/                       vendor material and the unpatched library backup
docs/ARCHITECTURE.md               this document
docs/BRINGUP.md                    power-up troubleshooting handbook (engineering)
docs/TESTER-HANDOFF.md             ★ the copy to hand to testers (no technical background assumed)
```
