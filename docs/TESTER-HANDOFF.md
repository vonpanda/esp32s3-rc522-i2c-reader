# Tester Hand-off — HS-S62A-PL RFID Reader Board

**English** ｜ [中文](TESTER-HANDOFF.zh-CN.md)

For whoever is handed the board for testing. **You do not need to understand the firmware
and you do not need to compile anything — just plug it in and watch the serial output.**

> **Note on log samples**: the firmware prints Chinese. Log lines are reproduced verbatim
> (they are what you will actually see on screen); English glosses are given underneath.

---

## 1. What this board can test

Card reading. Hold a card near the reader and the serial output prints the card number and
whether it is permitted.

The current firmware does exactly two things: **read cards** and **print what it is doing to
the serial port**. There is no relay and no networking, so "card read OK" means the whole
chain works.

---

## 2. Wiring (only 4 wires)

| Module silkscreen | Connect to ESP32-S3 | Note |
|---|---|---|
| G | GND | mandatory |
| V | 3V3 | 3.3–5 V; the module has an on-board regulator |
| SDA | **GPIO18** (header silkscreen "18") | wrong in = no card reads |
| SCL | **GPIO17** (header silkscreen "17") | wrong in = no card reads |

> ⚠️ **17 and 18 are adjacent on the header** — do not land one pin off.
> Getting them backwards will not burn anything, but it will not read cards. The firmware
> detects and prints this case at boot (see section 5).

---

## 3. How to connect the serial port

The board has **two USB ports and both show the log**, with identical content. Use whichever
is convenient:

| Port | Device name on your system | Note |
|---|---|---|
| Native USB | macOS `/dev/cu.usbmodem*`, Windows `COM*` | some computers need a driver |
| On-board USB-UART | macOS `/dev/cu.usbserial*` / `SLAB_USBtoUART` | goes through CH340/CP2102, usually driver-free |

Serial parameters are fixed: **115200, 8 data bits, no parity, 1 stop bit, no flow control**.

> Cross-check: plug in both ports and open two serial windows — the content should be
> identical. If only one port produces output, the driver for the other one is missing.

---

## 4. What you should see when everything is normal

**The moment you power it up:**

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
[       1][SELF  ] I2C 引脚自检通过  SDA=GPIO18 SCL=GPIO17  无短路
[       2][BOOT  ] reset_reason=1(POWERON) 连续复位计数=1
[      45][RFID  ] 上电: 挂载成功 addr=0x28 chip=SI522A @0x28 (0x92)
[     120][BOOT  ] 读卡器就绪  addr=0x28  SI522A @0x28 (0x92)
[     121][BOOT  ] 可以开始测试：请把卡片靠近读卡器
```

Three key lines, all of them required:

| Line | Meaning |
|---|---|
| `SELF  ] I2C 引脚自检通过` | wiring OK, pins healthy, no solder short |
| `RFID  ] 上电: 挂载成功 addr=0x28` | the reader chip was found |
| `BOOT  ] 可以开始测试` | everything is ready |

> *In English: "I2C pin self-check passed", "mount succeeded at address 0x28", "you may begin
> testing: hold a card near the reader".*

**Tapping a card (hold it against the module, 0–2 cm):**

```
[    3421][CARD  ] >>> 读卡成功  UID = 27 1F BE 06    sak=0x08  [放行 ALLOW]
[    4288][CARD  ] <<< 卡片移开  UID = 27 1F BE 06    在位 867 ms  读取 29 次
```

> *First line: read OK, UID `27 1F BE 06`, permitted (`放行 ALLOW`). Second line: card removed,
> present for 867 ms, read 29 times.*

**One status line every 10 seconds:**

```
[   10000][STATE ] 运行 10s | 进卡 1 离卡 1 拒绝 0 丢弃 0 | 链路 UP  addr=0x28 SI522A @0x28 (0x92)
```

> *Uptime 10 s | cards in 1, cards out 1, denied 0, dropped 0 | link UP.*

---

## 5. How to interpret problems

**The firmware tells you where the problem is — no guessing needed.** Check it against this table:

| What you see on serial | Meaning | What to do |
|---|---|---|
| `SELF ] !! I2C 引脚自检不合格` | pin shorted to ground / output stage dead / wire broken | check whether SDA and SCL are in the wrong holes and whether a solder joint is dry |
| `SELF ] !! SDA 与 SCL 短路（锡桥）` | the two wires are soldered together | clean the solder joint |
| `SELF ] I2C 引脚自检通过` but `全总线扫描无任何应答` | bus is fine but nobody is there | measure for 3.3 V between the module's V and G; confirm SDA/SCL are not swapped |
| `上电: 0x28 无应答 (endTransmission=2 NACK_ADDRESS)` | same as above (bus fine, module not answering) | same as above |
| `endTransmission=5 TIMEOUT` | physical-layer problem | module unpowered / no pull-up / a line held low |
| `链路 DOWN` or `LOST` | the link dropped mid-run | wiggle the ribbon to see if it recovers; the firmware retries by itself, no restart needed |
| `WARN ] 栈水位偏低` | a firmware memory-configuration issue, not a hardware issue | send us that line verbatim |

> *In English, the rows above read: "pin self-check failed", "SDA and SCL are shorted (solder
> bridge)", "pin self-check passed" but "full-bus scan found no response", "0x28 not responding
> (NACK_ADDRESS)", "link DOWN or LOST", "stack watermark low".*

### About "swapped"

**Swapping SDA and SCL will not burn anything**, but it presents very deceptively: bus levels,
pull-ups and waveform are all normal; it is just that no address answers (the error codes are
uniformly `NACK_ADDRESS`). In that case the firmware's diagnosis line names "swapped" as the
first suspect — just swap the two wires as it suggests.

---

## 6. Suggested test steps

1. Connect the 4 wires, plug in USB, open the serial port (115200)
2. Confirm all three key lines from section 4 appear
3. Tap **every card you can find** once each; note which ones read, which do not, and what UID each one reports
4. Test the reading distance: move a card from far to near and note the distance at which it first reads
5. Tap rapidly 20 times in a row; check that every tap is reported, with no duplicates or omissions
6. Unplug and replug the ribbon 3–5 times; check that it recovers by itself after a dropout (should recover within 10 seconds at most)
7. Leave it powered and running for a while (a few hours is suggested) and check that the `STATE` line's card in/out counters accumulate correctly

---

## 7. What to report back

When something goes wrong, just paste back **the full serial output from power-on up to the
point where the problem appeared** — you do not need to analyse it yourself. It already
contains everything needed to locate the issue (pin self-check, scan results, error codes,
diagnosis).

Please also mention: **where the power comes from** (USB port / external supply), **how long
the ribbon is**, and **which card type** you used.

---

## Appendix: if you want to reflash the firmware yourself

```bash
# Production / test firmware
pio run -e rymcu-esp32-s3-devkitc-1 -t upload

# Hardware self-check firmware (no business logic; reports wiring / pins / chip item by item — use this when troubleshooting)
pio run -e bringup -t upload

# Watch the serial port
pio device monitor -b 115200
```

The pin definitions exist in exactly one place, `src/app_config.h`. If you change the wiring,
change it there — do not hardcode it anywhere else.
