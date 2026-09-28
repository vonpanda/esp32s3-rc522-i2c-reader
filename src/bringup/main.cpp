/**
 * bringup/main.cpp —— 硬件 bring-up 固件（不跑业务，只做硬件自检）
 *
 *     pio run -e bringup -t upload
 *
 * 它要回答的问题只有三个：
 *   1. 模块到底有没有上电、SDA/SCL 有没有外部上拉？
 *   2. 模块在哪个 I2C 地址上？SDA/SCL 是不是接反了？
 *   3. 芯片是什么型号？读卡链路能不能通？
 *
 * 与生产固件的区别：这里刻意不用 FreeRTOS 任务、不联网、不看门狗，
 * 任何人拿到板子插上串口就能看懂输出。
 */
#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#include "app_config.h"
#include "app_log.h"
#include "i2c_bus.h"
#include "rfid_reader.h"

namespace {

struct ScanResult {
    uint8_t  found[8];
    uint8_t  foundCount;
    uint16_t ok;
    uint16_t nack;
    uint16_t timeout;
    uint16_t other;
};

// ---------------------------------------------------------------- 单根线检查
/** 引脚健康自检的展示层。
 *
 * 测量本身在总线层 `i2cbus::checkPin()` —— 生产固件启动自检用的是同一个函数，
 * 保证两边的判据永远一致（这个项目已经吃过"同一逻辑两处实现导致判据漂移"的亏）。
 * 这里只负责把四态翻译成人话。
 *
 * @return 这一根线是否基本健康。供锡桥检查决定要不要继续往下判。
 */
bool reportPinHealth(const char *name, int pin) {
    const i2cbus::PinHealth h = i2cbus::checkPin(pin);

    const char *verdict;
    if (h.pullUp == 0) {
        verdict = "!! 损坏/被钉死：该脚一直是低 —— 查连焊、锡桥，或该脚已击穿";
    } else if (h.drive0 == 1) {
        verdict = "!! 损坏：开漏拉不低 —— 输出级失效，这个脚不能用";
    } else if (h.hiZ == 0) {
        verdict = "引脚本身是好的，但这根线上没有外部上拉 -> 焊点/走线断路，或模块没接";
    } else if (h.pullDown == 0) {
        verdict = "可用但偏弱：无外部上拉，仅靠 ESP32 内部 ~45k 弱上拉";
    } else {
        verdict = "正常：输入、开漏输出、外部上拉全部到位";
    }

    appLogRaw("  %-4s GPIO%-2d  PULLUP=%d PULLDOWN=%d HiZ=%d DRIVE0=%d  => %s\n", name,
              pin, h.pullUp, h.pullDown, h.hiZ, h.drive0, verdict);
    return h.ok();
}

// ------------------------------------------------------------------ 地址扫描
ScanResult runScan() {
    ScanResult r = {};
    for (uint16_t a = CFG_RFID_SCAN_FIRST; a <= CFG_RFID_SCAN_LAST; ++a) {
        uint8_t e = i2cbus::probe(static_cast<uint8_t>(a));
        switch (e) {
            case 0:
                if (r.foundCount < sizeof(r.found)) {
                    r.found[r.foundCount] = static_cast<uint8_t>(a);
                }
                r.foundCount++;
                r.ok++;
                break;
            case 2:  r.nack++;    break;
            case 5:  r.timeout++; break;
            default: r.other++;   break;
        }
    }
    return r;
}

void printScanResult(const ScanResult &r) {
    appLogRaw("  错误码直方图:  OK=%u  NACK_ADDRESS=%u  TIMEOUT=%u  OTHER=%u\n",
              r.ok, r.nack, r.timeout, r.other);

    if (r.foundCount > 0) {
        appLogRaw("  命中设备: ");
        uint8_t shown = (r.foundCount < sizeof(r.found)) ? r.foundCount
                                                         : (uint8_t)sizeof(r.found);
        for (uint8_t i = 0; i < shown; ++i) {
            appLogRaw("0x%02X ", r.found[i]);
        }
        if (r.foundCount > shown) {
            appLogRaw("(另有 %u 个未列出)", (unsigned)(r.foundCount - shown));
        }
        appLogRaw("\n");
        return;
    }

    appLogRaw("  命中设备: 无\n");

    // 下面这段是整个 bring-up 固件的核心价值：把错误码翻译成硬件动作。
    if (r.timeout > 0 && r.nack == 0 && r.other == 0) {
        appLogRaw("\n  >>> 结论: 所有地址都返回 TIMEOUT(5)。\n");
        appLogRaw("      含义: I2C 事务根本没能在总线上完成。\n");
        appLogRaw("      注意: 地址写错只会返回 NACK_ADDRESS(2)，不会返回 5。\n");
        appLogRaw("      按现场概率依次排查:\n");
        appLogRaw("        [1] 模块 VCC 和 GND 是否真的接上、是否 3.3~5V。\n");
        appLogRaw("            只接 SDA/SCL 不接 V 是最常见的坑 —— 此时两根线\n");
        appLogRaw("            仍会被上拉读成高，看起来像「连线正常」。\n");
        appLogRaw("        [2] SDA/SCL 是否接反 —— 见下一段自动互换测试。\n");
        appLogRaw("        [3] 有没有 4.7k 外部上拉。RC522 模块板载通常没有，\n");
        appLogRaw("            100kHz 下只靠内部 45k 上拉边沿太慢，从机会采错位。\n");
        appLogRaw("        [4] 排线过长 / 杜邦线接触不良 / 某根线被拉死。\n");
        appLogRaw("        [5] 模块需要断电重新上电。厂商库 README 记录的已知问题。\n");
    } else if (r.nack > 0 && r.timeout == 0) {
        appLogRaw("\n  >>> 结论: 全部返回 NACK_ADDRESS(2)。\n");
        appLogRaw("      含义: 总线电气上完全正常，但没有任何从机应答。\n");
        appLogRaw("      排查: 模块未供电、模块损坏，或地址不在扫描区间内。\n");
        appLogRaw("            本模块手册标称 0x28，SI522A-2F 变体为 0x2F。\n");
    } else {
        appLogRaw("\n  >>> 结论: 错误码混杂（既有 TIMEOUT 又有 NACK）。\n");
        appLogRaw("      含义: 总线时序处于临界状态，典型原因是上拉太弱或走线过长。\n");
        appLogRaw("      建议: SDA/SCL 各加一个 4.7k 上拉到 3.3V；缩短排线；\n");
        appLogRaw("            或把 CFG_I2C_FREQ_HZ 降到 50000 复测。\n");
    }
}

void printChipInfo(uint8_t addr) {
    appLogRaw("  读取 VersionReg(0x37) @0x%02X ...\n", addr);

    // 用一次纯寄存器读，判断是「能通信」还是「只有地址应答」
    Wire.beginTransmission(addr);
    Wire.write(0x37);
    uint8_t e = Wire.endTransmission(false);
    if (e != 0) {
        appLogRaw("    写寄存器地址失败 endTransmission=%u(%s)\n", e,
                  i2cbus::wireErrName(e));
        return;
    }
    uint8_t n = Wire.requestFrom(static_cast<int>(addr), 1);
    if (n != 1) {
        appLogRaw("    读数失败，requestFrom 返回 %u\n", n);
        return;
    }
    uint8_t v = static_cast<uint8_t>(Wire.read());
    appLogRaw("    VersionReg=0x%02X  => %s\n", v, rfid::versionName(v));

    switch (v) {
        case 0x92:
            appLogRaw("    提示: 0x92 对应 SI522A @0x28\n");
            break;
        case 0xB2:
            appLogRaw("    提示: 0xB2 对应 SI522A @0x2F，地址要用 0x2F\n");
            break;
        case 0x00:
        case 0xFF:
            appLogRaw("    提示: 寄存器读回全 0 或全 1，说明通信并未真正建立\n");
            break;
        default:
            break;
    }
}

} // namespace

void setup() {
    // 两路串口（原生 USB + UART0）由 appLogInit() 统一打开，内容完全一致。
    appLogInit();

    appLogRaw("\n");
    appLogRaw("=========================================================\n");
    appLogRaw(" HS-S62A-PL RFID  BRING-UP 固件（硬件自检，不跑业务）\n");
    appLogRaw("=========================================================\n");
    appLogRaw(" 日志通道: %s\n", appLogChannelName());
    appLogRaw(" 引脚定义: SDA=GPIO%d  SCL=GPIO%d   (唯一定义在 src/app_config.h)\n",
              CFG_PIN_I2C_SDA, CFG_PIN_I2C_SCL);
    appLogRaw(" reset_reason=%d\n\n", static_cast<int>(esp_reset_reason()));

    // ---- 1) 引脚健康自检：必须在 Wire.begin() 之前做 ----
    // 这一段专治"我怀疑某个脚坏了"。它替代原来那种「pinMode(INPUT_PULLUP)
    // 之后读电平」的无效调试：那样做两根线必然读 1，接没接模块都一样。
    appLogRaw("--- 1) 引脚健康自检（总线尚未启用）---\n");
    appLogRaw("     判据: PULLUP=1(没被钉低) DRIVE0=0(能拉低) HiZ=1(外部上拉可达)\n");
    const bool sdaOk = reportPinHealth("SDA", CFG_PIN_I2C_SDA);
    const bool sclOk = reportPinHealth("SCL", CFG_PIN_I2C_SCL);

    if (sdaOk && sclOk) {
        if (i2cbus::busShorted(CFG_PIN_I2C_SDA, CFG_PIN_I2C_SCL)) {
            appLogRaw("  !! SDA 与 SCL 被短路在一起（锡桥）-> I2C 必然不通，先排除它\n");
        } else {
            appLogRaw("  SDA/SCL 之间无短路（锡桥检查通过）\n");
        }
    } else {
        appLogRaw("  两线未同时满足基本判据，跳过锡桥检查（否则结论是二义的）\n");
    }
    appLogRaw("\n");

    // ---- 2) 按配置引脚扫描 ----
    appLogRaw("--- 2) 地址扫描 SDA=GPIO%d SCL=GPIO%d @%lukHz ---\n",
              CFG_PIN_I2C_SDA, CFG_PIN_I2C_SCL,
              static_cast<unsigned long>(CFG_I2C_FREQ_HZ / 1000));
    if (i2cbus::begin() != i2cbus::Init::OK) {
        appLogRaw("  Wire.begin() 失败，总线无法启用\n");
        return;
    }
    ScanResult r1 = runScan();
    printScanResult(r1);
    appLogRaw("\n");

    // ---- 3) 引脚互换重试：一次性排除「SDA/SCL 接反」这个高频故障 ----
    int  activeSda = CFG_PIN_I2C_SDA;
    int  activeScl = CFG_PIN_I2C_SCL;
    bool swapped   = false;

    if (r1.foundCount == 0) {
        appLogRaw("--- 3) 引脚互换重试 SDA=GPIO%d SCL=GPIO%d ---\n",
                  CFG_PIN_I2C_SCL, CFG_PIN_I2C_SDA);
        i2cbus::begin(CFG_PIN_I2C_SCL, CFG_PIN_I2C_SDA, CFG_I2C_FREQ_HZ);
        ScanResult r2 = runScan();
        printScanResult(r2);

        if (r2.foundCount > 0) {
            swapped   = true;
            activeSda = CFG_PIN_I2C_SCL;
            activeScl = CFG_PIN_I2C_SDA;
            appLogRaw("\n  >>> 互换后能扫到设备 => SDA/SCL 接反了。\n");
            appLogRaw("      后续第 4、5 步【继续沿用电引脚】跑完，用来自证模块本身是好的。\n");
            appLogRaw("      看完结果后请【断电】把两根信号线对调，再重跑一次本固件确认。\n");
        } else {
            appLogRaw("\n  互换后仍无设备 => 可以排除接反的可能\n");
            // 只有确认没接反，才恢复成配置引脚，后续步骤才有一致的前提
            i2cbus::begin();
        }
        appLogRaw("\n");
    }

    // ---- 4) 芯片型号确认 ----
    appLogRaw("--- 4) 芯片确认 (SDA=GPIO%d SCL=GPIO%d) ---\n", activeSda, activeScl);
    const uint8_t targets[2] = {CFG_RFID_ADDR_PRIMARY, CFG_RFID_ADDR_ALT};
    bool anyAck = false;
    for (uint8_t i = 0; i < 2; ++i) {
        uint8_t e = i2cbus::probe(targets[i]);
        appLogRaw("  0x%02X probe=%u(%s)\n", targets[i], e, i2cbus::wireErrName(e));
        if (e == 0) {
            printChipInfo(targets[i]);
            anyAck = true;
        }
    }
    appLogRaw("\n");

    // ---- 5) 走一遍正式驱动的挂载路径 ----
    appLogRaw("--- 5) 正式驱动挂载测试 ---\n");
    bool ok = rfid::begin();
    appLogRaw("  rfid::begin() => %s\n", ok ? "成功" : "失败");
    if (ok) {
        appLogRaw("  addr=0x%02X chip=%s\n", rfid::info().addr,
                  rfid::versionName(rfid::info().version));
    } else {
        appLogRaw("  驱动无法挂载：%s\n", rfid::lastDiagnosis());
    }
    appLogRaw("\n");

    // ---- 6) 结论 ----
    appLogRaw("=========================================================\n");
    if (ok && swapped) {
        appLogRaw(" 结论: 模块完好，只是两根信号线接反了。\n");
        appLogRaw("       现在接法: 模块SDA->GPIO%d, 模块SCL->GPIO%d  (错)\n", activeSda,
                  activeScl);
        appLogRaw("       正确接法: 模块SDA->GPIO%d, 模块SCL->GPIO%d\n", CFG_PIN_I2C_SDA,
                  CFG_PIN_I2C_SCL);
        appLogRaw("       动作: 断电 -> 对调两根信号线 -> 重跑本固件，应直接在第 2 步命中。\n");
    } else if (ok) {
        appLogRaw(" 结论: 接线正确，模块已挂载成功。\n");
        appLogRaw("       动作: 上生产固件  pio run -e rymcu-esp32-s3-devkitc-1 -t upload\n");
    } else if (anyAck) {
        appLogRaw(" 结论: 地址有应答但驱动挂不上 -> 大概率是上拉不足或模块欠压。\n");
        appLogRaw("       动作: SDA/SCL 各加 4.7kΩ 上拉到 3.3V；量寻卡瞬间模块 V 是否跌落。\n");
    } else {
        appLogRaw(" 结论: 总线上没有任何器件应答 -> 模块侧硬件问题。\n");
        appLogRaw("       依据: %s\n", rfid::lastDiagnosis());
        appLogRaw("       动作: 万用表量模块 V-GND 电压；蜂鸣档逐根点通断。\n");
    }
    appLogRaw("=========================================================\n");
    appLogRaw("\n");
}

void loop() {
    static uint32_t lastRescanMs = 0;
    static uint32_t lastBeatMs = 0;
    const uint32_t now = millis();

    if (rfid::info().ready) {
        const uint8_t *uid = nullptr;
        uint8_t len = 0;
        uint8_t sak = 0;
        rfid::Poll r = rfid::poll(&uid, &len, &sak);

        if (r == rfid::Poll::CARD) {
            char s[24] = {};
            for (uint8_t i = 0; i < len; ++i) {
                snprintf(s + i * 2, sizeof(s) - i * 2, "%02X", uid[i]);
            }
            appLogRaw("  [CARD] uid=%s  sak=0x%02X  len=%u\n", s, sak, len);
        } else if (r == rfid::Poll::LINK_DOWN) {
            // 单次判活失败不算掉线 —— 链路是否真的断了由 rfid 层判定
            // （连续 CFG_LINK_FAIL_STREAK 次失败才翻成 LOST）。
            // 这里如果按"一失败就自愈"，一次偶发 NACK 就会触发一轮完整重初始化。
            if (rfid::linkState() != rfid::Link::UP) {
                appLogRaw("  [LINK] 连续判活失败，尝试自愈...\n");
                rfid::heal();
            }
        }
        delay(CFG_RFID_POLL_MS);
    } else {
        // 未挂载：每 5 秒重扫一次，方便插拔排线时实时看到现象。
        // 注意这里打的是"本次重扫"的分布 + 结论，不是累计计数 ——
        // 累计值会被此前不同引脚配置下的尝试污染（踩过这个坑）。
        if (now - lastRescanMs > 5000) {
            lastRescanMs = now;
            uint8_t found[8] = {};
            i2cbus::ErrTally t = {};
            uint8_t n = i2cbus::scan(found, sizeof(found), &t);
            if (n > 0) {
                appLogRaw("\n  重扫到 %u 个设备，尝试挂载...\n", n);
                rfid::begin();
                if (rfid::info().ready) {
                    appLogRaw("  挂载成功 addr=0x%02X chip=%s\n",
                              rfid::info().addr,
                              rfid::versionName(rfid::info().version));
                }
            } else {
                appLogRaw("\n  重扫 %u 个地址: ok=%u nack=%u timeout=%u\n     => %s\n",
                          t.total(), t.ok, t.nack, t.timeout, i2cbus::diagnose(t));
            }
        }
        if (now - lastBeatMs > 1000) {
            lastBeatMs = now;
            appLogRaw(".");
        }
        delay(50);
    }
}
