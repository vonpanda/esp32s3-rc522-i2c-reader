# ESP32-S3 + HS-S62A-PL RFID Reader Firmware

**Production-minded I2C firmware for the HS-S62A-PL 13.56 MHz RFID/NFC module (MFRC522 / SI522A compatible) on ESP32-S3.**

![PlatformIO](https://img.shields.io/badge/PlatformIO-espressif32%406.11.0-orange)
![Framework](https://img.shields.io/badge/framework-Arduino-blue)
![Board](https://img.shields.io/badge/board-ESP32--S3--DevKitC--1--N8R2-red)
![License](https://img.shields.io/badge/license-MIT-green)

The vendor example for this module is a `setup()` that calls `PCD_Init()` and a `loop()` that prints a UID. That is fine on a bench with a short jumper wire. It falls apart the moment the board goes into somebody else's hands, because **wiring wrong, module dead, and card simply absent all produce the same symptom: nothing prints.** On a cold boot with awkward power sequencing, the vendor driver can also wedge itself in an unbounded busy-wait loop and never return.

This firmware exists for the moment when a colleague says *"it doesn't read cards."* It turns that silence into a diagnosis.

## What you get over the vendor example

| Failure mode | Vendor example | This firmware |
|---|---|---|
| `SDA`/`SCL` swapped | looks identical to a dead module | pin self-check at boot; bring-up firmware auto-retries with the pins swapped |
| Module not powered | silent | `NACK(2)` vs `TIMEOUT(5)` histogram names it |
| Cold-boot wedge in `PCD_Reset()` | unbounded `while` → task hangs forever | patched to bounded retry ([`PATCHES.md`](lib/YFROBOTRFIDI2C/PATCHES.md)) |
| "no card" vs "reader offline" | both collapse into "read failed" | explicit `Link` state machine: `DOWN` / `LOST` / `UP` |
| Card removal | `REQA` cannot see a halted card, so removal is never reported | `WUPA` + `HaltA`, making presence a real edge event |
| Watchdog / crash loop | none | task WDT + RTC-domain boot counter → safe mode |
| Which serial port | one hardcoded port | USB-CDC **and** UART0, byte-identical output |

## Documentation

| Document | Audience |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Design rationale, failure-mode catalogue, timing budget, acceptance checklist |
| [`docs/BRINGUP.md`](docs/BRINGUP.md) | Power-up troubleshooting handbook — how to read each of the six bring-up steps |
| [`docs/TESTER-HANDOFF.md`](docs/TESTER-HANDOFF.md) | Hand-off notes for whoever holds the board; assumes no firmware knowledge |

All three have Chinese originals (`*.zh-CN.md`), cross-linked at the top of each page.
Log samples inside them stay verbatim (the firmware prints Chinese) with English glosses
in the surrounding text.

## Hardware

| | |
|---|---|
| Board | RYMCU ESP32-S3-DevKitC-1-N8R2 (8 MB QD flash, 2 MB quad PSRAM) — `board = rymcu-esp32-s3-devkitc-1` |
| Reader module | HS-S62A-PL (MFRC522 / SI522A compatible), 13.56 MHz |
| Interface | I2C, 100 kHz default (module is rated to 400 kHz) |
| Module address | `0x28` (default), or `0x2F` on the SI522A-2F variant |

### Wiring

| Module | ESP32-S3 | DevKitC-1 J1 | Notes |
|---|---|---|---|
| `SDA` | **GPIO18** | pin 11 | |
| `SCL` | **GPIO17** | pin 10 | |
| `VCC` | `3V3` | | 3.3–5 V per module spec |
| `GND` | `GND` | | **must be connected** — with only `SDA`/`SCL` wired, both lines idle high through the pull-ups and look "fine" |

> ### ⚠️ GPIO18/GPIO17 is not the conventional mapping
> The community default for this module is GPIO5/GPIO4. This project uses **GPIO18/GPIO17**, which is what was verified on the bench and is now the single source of truth in [`src/app_config.h`](src/app_config.h). If you lay out a PCB or crimp a ribbon cable, **silkscreen it exactly as above** — otherwise you reintroduce the "swapped → whole-bus NACK" failure this project exists to prevent.

Pins deliberately avoided on ESP32-S3: `GPIO0/3/45/46` (strapping), `GPIO19/20` (native USB), `GPIO38` (RGB LED on DevKitC v1.1), `GPIO39–42` (JTAG), `GPIO43/44` (UART0). On **octal-PSRAM** parts `GPIO35–37` are also claimed internally; this board is **quad**-PSRAM, so they are free.

## Quick start

```bash
git clone https://github.com/vonpanda/esp32s3-rc522-i2c-reader.git
cd esp32s3-rc522-i2c-reader

# 1) Bring-up firmware: verify wiring, address and chip ID. No business logic.
pio run -e bringup -t upload
pio device monitor -b 115200

# 2) Production firmware
pio run -e rymcu-esp32-s3-devkitc-1 -t upload
pio device monitor -b 115200
```

Toolchain is pinned: `platform = espressif32@6.11.0` (Arduino-ESP32 2.0.17), `framework = arduino`. There is **no `@latest` anywhere** on purpose — a production line that rebuilds at a different time must not silently get a different binary.

## Reading the output

Both serial ports are opened simultaneously and carry identical text, so whoever picks up the board can use whichever port they happen to have. On macOS that is `/dev/cu.usbmodem*` (native USB) and `/dev/cu.usbserial*` (on-board USB-UART bridge).

### Boot

```
=========================================================
 HS-S62A-PL 读卡器   ESP32-S3   测试固件 v1.1.0
=========================================================
 I2C 100 kHz   SDA=GPIO18   SCL=GPIO17   轮询 30 ms
 引脚定义唯一来源: src/app_config.h
 日志通道: USB + UART0(TX43/RX44)
---------------------------------------------------------
 操作方法: 把卡片靠近读卡器（有效距离 0~2cm）
           成功时打印 ">>> 读卡成功"，移开时打印 "<<< 卡片移开"
           每 10 秒打印一行 STATE 状态；无异常时不会有多余输出
=========================================================
[     512][SELF  ] I2C 引脚自检通过  SDA=GPIO18 SCL=GPIO17  无短路
[     514][BOOT  ] reset_reason=1(POWERON) 连续复位计数=1
[     530][BOOT  ] 读卡器就绪  addr=0x28  SI522A @0x28 (0x92)
[     532][BOOT  ] 可以开始测试：请把卡片靠近读卡器
[     533][BOOT  ] 任务已启动 heap=312456
```

Every tagged line is `[millis][TAG] message`. Tags: `SELF` `BOOT` `CARD` `STATE` `RFID` `WARN` `WDT` `SAFE`.

### Card tap

```
[   14200][CARD  ] >>> 读卡成功  UID = 27 1F BE 06    sak=0x08  [放行 ALLOW]
[   18700][CARD  ] <<< 卡片移开  UID = 27 1F BE 06   在位 4500 ms  读取 150 次
```

`UID` is byte-spaced so it can be read straight off a card or a PC tool without mental hex-splitting. `[放行 ALLOW]` / `[拒绝 DENY ]` comes from the whitelist hook at the bottom of `src/main.cpp` — replace it with NVS or a backend lookup and the interface does not move.

### Heartbeat

```
[   10533][STATE ] 运行 10s | 进卡 0 离卡 0 拒绝 0 丢弃 0 | 链路 UP  addr=0x28 SI522A @0x28 (0x92)
```

One line every 10 s answers "can it still read, how many has it read, is the link alive". Healthy operation prints nothing else — so a `WARN` line appearing in the log is itself a signal, rather than being buried in routine noise.

### Reader absent — the common bench case

```
[     534][RFID  ] 读卡器缺席（从未挂载）-> 进入低速重试
[    5000][RFID  ] 缺席 第 3 次恢复失败，2000ms 后重试 | 总线电气正常但无任何地址应答：首选怀疑 SDA/SCL 接反
```

A missing reader is a *normal* state, not a reason to spam. The task backs off 250 ms → 500 → 1 s → 2 s → 4 s → 8 s → 10 s (capped) and throttles its own log to one line per 5 s. State *changes* (absent ↔ lost ↔ recovered) always print immediately, unsuppressed.

## Bring-up firmware

`pio run -e bringup -t upload` runs a six-step hardware check with no RTOS, no Wi-Fi and no watchdog. Plug in a serial cable and the output is readable by anyone — that is the whole point.

```
--- 1) 引脚健康自检（总线尚未启用）---
     判据: PULLUP=1(没被钉低) DRIVE0=0(能拉低) HiZ=1(外部上拉可达)
  SDA  GPIO18  PULLUP=1 PULLDOWN=1 HiZ=1 DRIVE0=0  => 正常：输入、开漏输出、外部上拉全部到位
  SCL  GPIO17  PULLUP=1 PULLDOWN=1 HiZ=1 DRIVE0=0  => 正常：输入、开漏输出、外部上拉全部到位
  SDA/SCL 之间无短路（锡桥检查通过）

--- 2) 地址扫描 SDA=GPIO18 SCL=GPIO17 @100kHz ---
  错误码直方图:  OK=0  NACK_ADDRESS=112  TIMEOUT=0  OTHER=0
  命中设备: 无

  >>> 结论: 全部返回 NACK_ADDRESS(2)。
      含义: 总线电气上完全正常，但没有任何从机应答。
      排查: 模块未供电、模块损坏，或地址不在扫描区间内。
            本模块手册标称 0x28，SI522A-2F 变体为 0x2F。

--- 3) 引脚互换重试 SDA=GPIO17 SCL=GPIO18 ---
  错误码直方图:  OK=1  NACK_ADDRESS=111  TIMEOUT=0  OTHER=0
  命中设备: 0x28

  >>> 互换后能扫到设备 => SDA/SCL 接反了。
      后续第 4、5 步【继续沿用电引脚】跑完，用来自证模块本身是好的。
      看完结果后请【断电】把两根信号线对调，再重跑一次本固件确认。

--- 5) 正式驱动挂载测试 ---
  rfid::begin() => 成功
  addr=0x28 chip=SI522A @0x28 (0x92)

=========================================================
 结论: 模块完好，只是两根信号线接反了。
       正确接法: 模块SDA->GPIO18, 模块SCL->GPIO17
       动作: 断电 -> 对调两根信号线 -> 重跑本固件，应直接在第 2 步命中。
=========================================================
```

### Interpreting the error codes

`endTransmission()` return values are translated into things you can act on:

| Code | Name | Means | Where to look |
|---|---|---|---|
| `0` | `OK` | device ACKed | — |
| `2` | `NACK_ADDRESS` | bus is electrically fine, nobody answers at that address | module unpowered / dead, or wrong address (`0x28` vs `0x2F`) |
| `5` | `TIMEOUT` | the transaction never completed | **`SDA`/`SCL` held low, no external pull-up, module not powered** |
| `1` / `3` / `4` | `TX_BUFFER_TOO_LONG` / `NACK_DATA` / `OTHER_ERROR` | driver-level | — |

**The one non-obvious result from the field** — a swapped `SDA`/`SCL` pair produces a **clean all-`NACK` histogram, not timeouts.** Both lines still idle high, both have working pull-ups, every transaction completes normally; the module simply never hears its own address. So if you are staring at `NACK_ADDRESS=112` with the module definitely powered, fix the pin mapping before you unsolder anything. This project spent a full debugging round learning that, and it is why the bring-up firmware scans a second time with the pins swapped.

### Pin four-state check

Each line is measured as a plain GPIO *before* the I2C peripheral takes it over:

| Reading | Meaning |
|---|---|
| `PULLUP=0` | the pin is stuck at GND — solder bridge, or the pin is blown |
| `DRIVE0=1` | open-drain cannot pull low — output stage dead, the pin is unusable |
| `HiZ=0` | no external pull-up reachable — **open pad, broken trace, or module not connected** |
| `PULLDOWN=0` | usable but weak — running on the ESP32's internal ~45 kΩ only |

Note that `DRIVE0` **cannot** detect an open pad: with the pad disconnected, the output stage still pulls down the short die-side stub and reads back `0`. `HiZ` is what catches it, because no external pull-up is reachable any more.

On top of the per-pin check there is a dedicated **solder-bridge test** between `SDA` and `SCL`: drive one line open-drain low, hold the other in `INPUT_PULLUP`, and see whether it gets dragged along — in both directions. This has to be a separate test, because two lines soldered into one net look *individually perfect*: correct levels, pull-ups present, able to pull low. Only the cross-check sees it. After re-soldering or re-crimping a ribbon this is the most common failure, and `SDA`/`SCL` are usually adjacent pins.

## Architecture

Full design rationale and failure-mode catalogue: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

```
                Core 1                                Core 0
  ┌──────────────────────────────┐     ┌──────────────────────────────┐
  │ rfidTask    prio 6   4 KB    │     │ reportTask   prio 3   6 KB   │
  │   sole I2C owner             │     │   business / output          │
  ├──────────────────────────────┤     │   may block indefinitely     │
  │ healthTask  prio 2   4 KB    │     └──────────────────────────────┘
  │   stats only, never I2C      │                    ▲
  └──────────────────────────────┘                    │
                  │        CardEvent queue (depth 8)  │
                  └───────────────────────────────────┘
```

Three rules hold the design together:

1. **Only `rfidTask` touches I2C.** A single writer removes bus contention and priority inversion by construction — there are no locks on the hot path.
2. **Only `reportTask` may block indefinitely** (`xQueueReceive`). Every other task returns to the scheduler each iteration.
3. **No allocation after init.** Queues, buffers and driver objects are statically allocated.

Layer boundaries: `i2c_bus` (bytes on a wire — knows nothing about RC522) → `rfid_reader` (device semantics: mount / poll / recover, and the *sole* judge of link health) → `main` (orchestration, IPC, watchdog).

### Tuning

Every tunable lives in [`src/app_config.h`](src/app_config.h); no bare magic numbers are permitted elsewhere. The ones you will actually reach for:

| Define | Default | Why you'd change it |
|---|---|---|
| `CFG_PIN_I2C_SDA` / `_SCL` | `18` / `17` | your wiring differs |
| `CFG_I2C_FREQ_HZ` | `100000` | raise to `400000` once the link is proven stable |
| `CFG_RFID_POLL_MS` | `30` | must exceed the blocking length of one no-card poll |
| `CFG_RC522_TRELOAD` | `512` (≈12.8 ms) | raise to `1000` if you see missed taps — that is the module default of 25 ms |
| `CFG_CARD_RELEASE_MS` | `500` | card-removal debounce |
| `CFG_CARD_REEMIT_MS` | `2000` | re-report interval for a card left on the reader; `0` = report on entry only |
| `CFG_STATE_LINE_MS` | `10000` | heartbeat period; set to `1000` while developing |
| `CFG_VERBOSE_HEALTH` | `0` | `1` appends stack/heap watermarks and the bus error histogram |
| `CFG_LOG_USB_CDC` / `CFG_LOG_UART0` | `1` / `1` | disable one channel to save ≈1.5 KB of flash |

Two of these are coupled in a way worth knowing before you touch them: `CFG_HEALTH_TICK_MS` doubles as the watchdog feed, so it must stay far below `CFG_TWDT_SECONDS`, whereas `CFG_STATE_LINE_MS` only controls printing. An earlier revision shared a single constant between the two, and raising it to 10 s made the watchdog fire first — a self-inflicted reset loop.

## Repository layout

```
src/
  app_config.h          every tunable, single source of truth
  app_log.{h,cpp}       dual-channel logging (USB-CDC + UART0)
  i2c_bus.{h,cpp}       bus layer: error classes, scan, pin self-check, line recovery
  rfid_reader.{h,cpp}   device layer: mount / poll / heal, sole owner of link state
  main.cpp              production firmware: tasks, queue, watchdog, whitelist hook
  bringup/main.cpp      bring-up firmware: six-step hardware check
lib/
  YFROBOTRFIDI2C/       vendored vendor driver, locally patched — see PATCHES.md
docs/
  ARCHITECTURE.md       design rationale and failure-mode catalogue
  BRINGUP.md            bring-up procedure, step by step
  TESTER-HANDOFF.md     hand-off notes for testers
  *.zh-CN.md            Chinese originals of the three documents above
```

Two build environments share one source tree, separated by `build_src_filter`, so bring-up code never leaks into the production binary and vice versa.

## Third-party code

`lib/YFROBOTRFIDI2C/` is the YFROBOT `YFROBOTRFIDI2C` library v1.0.3, **BSD-3-Clause**, redistributed under its original license (see [`lib/YFROBOTRFIDI2C/LICENSE`](lib/YFROBOTRFIDI2C/LICENSE)). It is **modified**: three unbounded busy-wait loops were converted to bounded ones, so a mis-sequenced power-up can no longer hang a watchdog-supervised task. Every change is documented in [`PATCHES.md`](lib/YFROBOTRFIDI2C/PATCHES.md) — use that file as the checklist if you ever upgrade the vendored library.

The pristine upstream copy is kept in `docs/vendor/YFROBOTRFIDI2C-1.0.3-original/` so the patches stay diffable. The rest of the manufacturer's material — roughly 150 MB of datasheets, archives and examples for other boards — is deliberately **not** committed; see [`docs/vendor/README.md`](docs/vendor/README.md).

Driver behaviour that is left alone, and the application-layer compensation for it:

| Behaviour | Compensated by |
|---|---|
| `PICC_RequestA` / `WakeupA` blocks until the chip's internal timer expires | `TReloadReg` lowered from 1000 to 512 |
| `PCD_WriteRegister` ignores `endTransmission()`'s return value | link liveness verified by reading back `VersionReg(0x37)` |
| not thread-safe | a single I2C owner for the whole process |
| `PCD_PerformSelfTest()` is meaningless on SI522A | never called; `VersionReg` is used instead |

## License

MIT — see [`LICENSE`](LICENSE). This covers the project's own source; the vendored library keeps its own BSD-3-Clause license.

---

## 中文速览

**ESP32-S3 + HS-S62A-PL（MFRC522 / SI522A）I2C 读卡器固件。**

厂商例程只有"初始化 + 打印 UID"两句话。接线错、模块坏、只是没放卡——这三种情况在现场的表现完全一样：什么都不打印。本固件专门解决这件事，把沉默变成一句能照着修的结论。

- **双串口同时输出**（原生 USB + UART0，内容完全一致），板子交给谁都能直接连上看
- **启动引脚自检**：四态判定（被钉低 / 输出级坏 / 无外部上拉）+ SDA-SCL 锡桥交叉检查，把"接线错"和"模块坏"区分开
- **`Link` 三态状态机**（`DOWN` / `LOST` / `UP`），"场上没卡"和"模块掉线"不再混为一谈
- **用 WUPA 而非 REQA 寻卡**，配合 `HaltA` 才能做出真正的"卡已移开"判断
- **链路缺席时指数退避**（250 ms → 10 s），不刷屏、不空转总线
- **任务看门狗 + RTC 域复位计数**，出现复位循环自动进安全模式
- **厂商库打补丁**：3 处无界忙等改为有界，避免上电时序异常时任务永久卡死

接线：模块 `SDA` → **GPIO18**，`SCL` → **GPIO17**，`VCC` → 3V3，`GND` → GND。
这不是惯例接法（惯例是 GPIO5/GPIO4），但已实测定版，**PCB 丝印必须按此标注**。

```bash
pio run -e bringup -t upload                    # 先跑硬件自检固件
pio run -e rymcu-esp32-s3-devkitc-1 -t upload   # 再上生产固件
pio device monitor -b 115200
```

所有可调参数集中在 `src/app_config.h`，其余文件不得出现裸魔数。

> 现场最容易踩的坑：**SDA/SCL 接反时错误码是清一色的 NACK(2)，不是 TIMEOUT(5)。**
> 两根线的电平和上拉都正常、事务也都正常完成，只是模块听不到自己的地址。
> 看到 `NACK_ADDRESS=112` 而模块供电正常，先怀疑接线方向，别急着换模块。
