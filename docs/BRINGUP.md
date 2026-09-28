# Power-up Troubleshooting Handbook — HS-S62A-PL RFID Module

**English** ｜ [中文](BRINGUP.zh-CN.md)

> Companion firmware: `pio run -e bringup -t upload`, then `pio device monitor -b 115200`.

> **Note on log samples**: the firmware prints Chinese. Log lines are reproduced
> verbatim (they are what you will actually see on the serial port); English
> glosses appear in the surrounding text.

---

## 0. First, the conclusion: why the original debug output carried no information

The original code:

```cpp
pinMode(SDA_PIN, INPUT_PULLUP);
pinMode(SCL_PIN, INPUT_PULLUP);
Serial.printf("Before Wire.begin: SDA=%d, SCL=%d\n", digitalRead(SDA_PIN), digitalRead(SCL_PIN));
...
Serial.printf("After Wire.begin:  SDA=%d, SCL=%d\n", ...);
Serial.printf("After transfer:   SDA=%d, SCL=%d\n", ...);
```

All three lines print `SDA=1, SCL=1`, and **that is a foregone conclusion — it is the
same whether or not the module is connected**:

- with the internal pull-up enabled, the pin reads 1 as long as the line is not shorted to ground;
- the module unpowered, SDA not connected, or the module not on the board at all — still 1.

So those three lines prove only "the line is not shorted" and discriminate nothing else.

**Three power-up checks** (wiring follows `CFG_PIN_I2C_SDA=18` / `CFG_PIN_I2C_SCL=17`,
i.e. module SDA → GPIO18, module SCL → GPIO17):

| Step | What to measure | How | Verdict |
|---|---|---|---|
| 1 | Is the module powered? | multimeter across the module's V and G | 3.3–5 V; if not, the supply is the root cause |
| 2 | Are there external pull-ups? | power off, measure resistance SDA–VCC and SCL–VCC | a few kΩ means yes; open circuit means no |
| 3 | Is the wiring right? | power off, continuity-check both ends | do not trust the colours — buzz it out |

---

## 1. Key: what `endTransmission()`'s return value means

`Wire::endTransmission()` return values in ESP32 Arduino core 2.0.17 (source `Wire.cpp:433`):

| Value | Meaning | What it points to |
|---|---|---|
| `0` | success | device present and ACKing |
| `1` | data too long | calling convention is wrong |
| `2` | **NACK_ADDRESS** | bus is electrically **fine**, but nothing ACKs this address → wrong address / module not running / module dead |
| `3` | NACK on data | register address not accepted |
| `4` | other | bad parameter |
| `5` | **TIMEOUT** | the transaction **never completed on the bus** → held low / no pull-up / unpowered / swapped pins |

### What your `endTransmission = 5` told us

**It is not "wrong address".** A wrong address — bus fine but nobody answering — returns `2`.

`5` is ESP-IDF's `ESP_ERR_TIMEOUT` (the mapping at `Wire.cpp:468`): the I2C transaction did
not complete before the software timeout. In descending order of field probability:

1. **Module VCC not connected** (most common). With only SDA/SCL/GND wired, SDA and SCL are
   still pulled high and it *looks* like the wiring is fine, but the module has no ability
   to answer at all.
2. **SDA/SCL swapped.**
3. **No external pull-up at all.** RC522 modules generally do not carry one; relying on the
   ESP32's internal ~45 kΩ puts the slave's sampling edge in the wrong place at 100 kHz.
4. **A line held low** (short, crushed jumper insulation, over-long ribbon).
5. **The module needs a power cycle.** The vendor library's README v1.0.3 states this
   explicitly:
   > 优化代码，尝试解决重新上电，异常初始化无法正常通讯。未发现程序问题，解决方法：断电重启。
   >
   > *(attempted to fix abnormal init after re-powering; no program fault found, workaround: power-cycle)*

> Also, the original code set the clock to `Wire.setClock(10000)` (10 kHz). The module is
> rated to 400 kHz, and 10 kHz is a malformed configuration: one byte takes 900 µs, and the
> ESP32 I2C controller's hardware timeout is derived from the configured frequency, so at
> very low rates it tends to time out first. This firmware uses 100 kHz throughout
> (`CFG_I2C_FREQ_HZ`).

---

## 2. How to read the bring-up firmware's output

It runs six steps in sequence, each with a verdict.

### Step 1 — Pin four-state check (including the solder-bridge test)

Each line is measured on four axes first, then the two lines are checked for having been
soldered together.

| Reading | Expected | Meaning |
|---|---|---|
| `PULLUP` | 1 | enabling the internal pull-up reads high. Getting 0 ⇒ the pin is pinned to GND (solder bridge / blown) |
| `DRIVE0` | 0 | open-drain can pull low. Getting 1 ⇒ **output stage failed, this pin is unusable** |
| `HiZ` | 1 | still high with internal pulls disabled ⇒ an external pull-up is reachable (pad bonding is intact) |
| `PULLDOWN` | 1 | even the internal ~45 kΩ pull-down cannot drag it low ⇒ a strong external pull-up exists (normal) |

Interpretation rules:

| Observation | Verdict |
|---|---|
| `PULLUP=1 DRIVE0=0 HiZ=1` | **normal** — all three states present |
| `PULLUP=0` | the pin is pinned low — check for a solder bridge; once soldering is ruled out, the pin is blown |
| `DRIVE0=1` | the pin's output stage is dead; move to another pin and update `app_config.h` |
| `HiZ=0` with `PULLUP=1` | **the pin itself is fine**, but no external pull-up is reachable ⇒ open pad / broken trace, or the module is not connected |
| `PULLDOWN=0` with `HiZ=1` | usable but weak: no external pull-up, relying on the internal ~45 kΩ |

The **solder-bridge test** (it only runs when both lines are individually healthy): drive one
line low open-drain while the other stays `INPUT_PULLUP`, and read it. If the other line
follows it low ⇒ the two lines are the same net ⇒ I2C cannot possibly work.

> Why a solder bridge needs its own test: when two lines are soldered together, **each one
> looks completely "normal" on its own** — correct level, pull-up present, able to pull low.
> Only cross-driving exposes it. After re-soldering this is the most common failure, and on
> this board J1's SDA/SCL pins (pin 11 / pin 10) are adjacent.

### Step 2 — Address scan with an error-code histogram

This is the most valuable step: it aggregates the probe results across 112 addresses into a
histogram.

- `OK=1 NACK=111 TIMEOUT=0` → found 0x28, normal.
- `OK=0 NACK=112 TIMEOUT=0` → bus is electrically fine but nobody is there. **Suspect swapped
  SDA/SCL first** (see step 3), and only then an unpowered or dead module. This is exactly
  what happened on this project.
- `OK=0 NACK=0 TIMEOUT=112` → **physical-layer problem**: held low / no pull-up / unpowered.
  Note 112 timeouts cost about 2.2 seconds (20 ms each), so the elapsed time alone identifies
  this case.
- Mixed → timing is marginal; add 4.7 kΩ pull-ups / shorten the ribbon / drop to 50 kHz.

### Step 3 — Automatic SDA/SCL swap retry

If step 2 found no device at all, the firmware scans again with the pins exchanged.
**Finding a device after the swap ⇒ swapped wiring is confirmed**, no more guessing.

Steps 4 and 5 then **continue with the working (swapped) pin pair** so the module can prove
itself healthy, and the conclusion block states the correct wiring. Once you have read the
result, power off, swap the two signal wires, and run it again to confirm.

### Step 4 — Chip identification

Reads `VersionReg` (register 0x37) — the only hard evidence that communication is genuinely
established:

| Read-back | Chip |
|---|---|
| `0x91` | MFRC522 v1.0 |
| `0x92` | SI522A (**address 0x28**) |
| `0xB2` | SI522A-2F (**address 0x2F**) |
| `0x88` | FM17522-compatible part |
| `0x82` | "new" MFRC522 |
| `0x00` / `0xFF` | **communication not established** (read back all zeros or all ones) |

The `0xB2` row is worth noting: the vendor library's default address is 0x28, but a 0x2F
variant exists in the same family. The production firmware already tries both addresses and
falls back to a full-bus scan.

### Step 5 — Real driver mount

Runs exactly the same mount path as the production firmware. Once this passes, you can go
back to the production firmware.

### Step 6 — Conclusion block

A boxed summary that names the correct wiring and the next action, for example:

```
=========================================================
 结论: 模块完好，只是两根信号线接反了。
       现在接法: 模块SDA->GPIO17, 模块SCL->GPIO18  (错)
       正确接法: 模块SDA->GPIO18, 模块SCL->GPIO17
       动作: 断电 -> 对调两根信号线 -> 重跑本固件，应直接在第 2 步命中。
=========================================================
```

> *Verdict: the module is fine, the two signal wires are simply swapped. Current (wrong):
> SDA→GPIO17, SCL→GPIO18. Correct: SDA→GPIO18, SCL→GPIO17. Action: power off → swap the two
> wires → re-run this firmware; it should hit at step 2 directly.*

---

## 3. Wiring (ESP32-S3 DevKitC-1 ↔ module)

| Module silkscreen | Connect to ESP32-S3 | Notes |
|---|---|---|
| G | GND | mandatory |
| V | 3V3 (or 5V) | 3.3–5 V; the module has an on-board regulator |
| SDA | **GPIO18** (J1 pin 11) | this is the value of `CFG_PIN_I2C_SDA` |
| SCL | **GPIO17** (J1 pin 10) | this is the value of `CFG_PIN_I2C_SCL` |

> The single source of truth is the two constants in `src/app_config.h`:
> `CFG_PIN_I2C_SDA=18`, `CFG_PIN_I2C_SCL=17`. In words: **ESP32 GPIO18 is SDA, GPIO17 is SCL.**
>
> ⚠️ This is not the conventional mapping for ESP32 (the usual choices are 5/4 or 8/9, and
> this project itself used 5/4 before 2026-09-28). It became 17/18 because the bench
> re-soldered the wires to those pins. Note that J1's pin 10 and pin 11 are **adjacent**,
> and adjacent SDA/SCL brings solder-bridge risk — bring-up firmware step 1 now tests for
> exactly that.
>
> **Making a PCB / crimping a ribbon**: the silkscreen must match the table above, or you
> will walk straight back into the "swapped → whole-bus NACK" trap. That failure is very
> deceptive — levels, pull-ups and waveform are all normal, the error codes are uniformly
> NACK, and it is easily misread as "the module is dead" or "the pin is dead". To settle it,
> run bring-up firmware step 3's automatic swap scan; it will confirm the answer for you.

**Pins to avoid**: GPIO0 / GPIO3 / GPIO45 / GPIO46 are strapping pins whose level at power-up
selects the boot mode; GPIO19 / GPIO20 are taken by native USB; GPIO38 is the v1.1 RGB LED;
GPIO39–42 are JTAG; GPIO43 / GPIO44 are UART0 (**both logging and flashing go through them —
disturb them and you lose all output**). GPIO35–37 are used internally on octal-PSRAM parts
(this project's N8R2 is quad, so they are free).

---

## 4. Common errors quick reference

Log text is given verbatim in Chinese, with the meaning in English.

| Symptom | Root cause | Action |
|---|---|---|
| `endTransmission = 5`, all addresses | unpowered / swapped / no pull-up / bus held low | work through section 1 in order |
| `endTransmission = 2`, 0x28, and a full scan finds nothing | bus electrically fine, nobody there | measure V–GND at the module; check whether SDA/SCL are swapped (should be SDA=GPIO18 / SCL=GPIO17) |
| step 1 reports `PULLUP=0` | the pin is pinned to GND | check for a solder bridge; if soldering is ruled out the pin is blown — move pins |
| step 1 reports `DRIVE0=1` | the pin's output stage failed | that pin is unusable; move to another pair and update `app_config.h` |
| step 1 reports `HiZ=0` | no external pull-up reachable | open pad / broken trace, or the module is not connected |
| step 1 reports `SDA 与 SCL 被短路` *(SDA and SCL are shorted)* | solder bridge between adjacent pins | **most common after re-soldering.** Clean it with desoldering braid; J1's 17/18 and 4/5 are both adjacent pairs |
| `VersionReg=0x00` or `0xFF` | the address ACKs but register reads do not work | almost certainly insufficient pull-up; add 4.7 kΩ |
| Intermittently dead after power-up, fixed by a power cycle | the vendor library's known power-sequencing issue | the production firmware heals automatically; cure it properly with a load switch |
| Reads work intermittently | card at the 2 cm boundary / insufficient pull-up | move closer; add pull-ups; set `CFG_RC522_TRELOAD` back to 1000 |
| Serial repeatedly prints the boot banner | reset loop | the production firmware enters safe mode on the 8th boot; check `reset_reason` |
| Log shows only "缺席 第 N 次恢复失败" *(absent, recovery attempt N failed)* with growing gaps | **this is designed behaviour**, not a fault | exponential backoff at work. Follow the diagnosis text in the log to check the hardware; do not change the firmware |
| `link=LOST` recurs, heals successfully then drops again | loose ribbon / module supply sag | check the ribbon; scope the module's V rail during a poll to see if it dips (antenna inrush) |

---

## 5. Switching back from bring-up to the production firmware

```bash
pio run -e rymcu-esp32-s3-devkitc-1 -t upload
pio device monitor -b 115200
```

A healthy boot looks like this (verbatim):

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
[   10000][STATE ] 运行 10s | 进卡 0 离卡 0 拒绝 0 丢弃 0 | 链路 UP  addr=0x28 SI522A @0x28 (0x92)
```

Line by line:

| Log line (translated) | Meaning |
|---|---|
| `SELF ] I2C 引脚自检通过 SDA=GPIO18 SCL=GPIO17 无短路` | pin self-check passed, no short |
| `BOOT ] reset_reason=1(POWERON) 连续复位计数=1` | reset cause POWERON, consecutive-reset counter 1 |
| `RFID ] 上电: 挂载成功 addr=0x28 chip=SI522A @0x28 (0x92)` | mount succeeded at 0x28, chip identified |
| `BOOT ] 读卡器就绪` / `可以开始测试：请把卡片靠近读卡器` | reader ready / you may start testing |
| `STATE ] 运行 10s \| 进卡 0 离卡 0 拒绝 0 丢弃 0 \| 链路 UP` | uptime 10 s; in/out/deny/drop = 0/0/0/0; link UP |

Note the `SELF` line: **the production firmware also runs the pin four-state check plus the
solder-bridge test at boot** (sharing the same implementation with the bring-up firmware —
`i2cbus::checkPin()` / `i2cbus::busShorted()` — so the criteria can never drift apart). That
way, when the board is handed to someone else, the three problems that normally all present
as "cannot read any card" — wrong wiring, broken wire, solder bridge — are told apart within
the first second.

Tapping a card (verbatim):

```
[    3421][CARD  ] >>> 读卡成功  UID = 27 1F BE 06    sak=0x08  [放行 ALLOW]
[    4288][CARD  ] <<< 卡片移开  UID = 27 1F BE 06    在位 867 ms  读取 29 次
```

> *`>>> 读卡成功` = read OK; `UID = 27 1F BE 06`; `sak=0x08`; `[放行 ALLOW]` = permitted.
> `<<< 卡片移开` = card removed; `在位 867 ms` = present for 867 ms; `读取 29 次` = read 29 times.*

> When handing this to a colleague for testing, just give them
> `docs/TESTER-HANDOFF.md` — it contains the wiring table, serial parameters, expected output,
> and a "if you see this, it means that" interpretation table.

If the first mount fails, note that the behaviour now is **exponential backoff retries plus a
one-line diagnosis**; you will no longer see the "hammer the bus every 30 ms + print a heal
failure every 150 ms" flooding. The example below uses the swapped-pins case actually
encountered in the field (all NACK, zero timeouts); the `X` placeholders are millisecond
timestamps:

```
[       X][RFID  ] 上电: 0x28 无应答 (endTransmission=2 NACK_ADDRESS)
[       X][RFID  ] 变体地址: 0x2F 无应答 (endTransmission=2 NACK_ADDRESS)
[       X][RFID  ] 全总线扫描无任何应答 (112 个地址) -> 总线电气正常但无任何地址应答：
                    首选怀疑 SDA/SCL 接反（接反时电平、上拉、波形全部正常，错误码清一色 NACK）；
                    其次才是模块未供电、模块损坏、或地址不在扫描区间。
[       X][BOOT  ] 读卡器未挂载，采集任务将以指数退避方式重试 (250ms -> 10s)。诊断：<同上>
[       X][RFID  ] 读卡器缺席（从未挂载）-> 进入低速重试
[       X][RFID  ] 缺席 第 1 次恢复失败，250ms 后重试 | <诊断结论>
[       X][RFID  ] 缺席 第 2 次恢复失败，500ms 后重试 | <诊断结论>
        ...（节流到每 5 s 一行，退避到 10 s 封顶后一直保持）
```

> *Translated: boot — 0x28 no response (NACK_ADDRESS); alternate address 0x2F no response;
> full-bus scan found nothing across 112 addresses → the bus is electrically fine but no
> address answers: suspect swapped SDA/SCL first (when swapped, levels/pull-ups/waveform are
> all normal and error codes are uniformly NACK); only then an unpowered module, a dead
> module, or an address outside the scan range. Reader not mounted; the acquisition task will
> retry with exponential backoff (250 ms → 10 s). Diag: same as above. Reader absent (never
> mounted) → entering slow retry. Absent: recovery attempt 1 failed, retrying after 250 ms;
> attempt 2 failed, retrying after 500 ms; … (throttled to one line per 5 s; holds at the
> 10 s cap).*

Once the module is connected properly **the firmware does not need a restart** — it picks it
up within at most 10 s:

```
[       X][RFID  ] 链路恢复 addr=0x28 chip=SI522A @0x28 (0x92)（累计尝试 7 次）
[       X][STATE ] 运行 ...s | 进卡 0 离卡 0 拒绝 0 丢弃 0 | 链路 UP  addr=0x28 SI522A @0x28 (0x92)
```

> *Link recovered at 0x28, chip SI522A, after 7 cumulative attempts.*

> The upper bound on noticing link recovery is `CFG_BACKOFF_TICK_MS` (50 ms by default). If
> you want it to reconnect faster after a dropout, lower `CFG_BACKOFF_MAX_MS` from 10000 to
> 2000 — at the cost of hitting the bus more often while the module is absent for a long time.
