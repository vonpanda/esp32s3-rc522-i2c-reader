/**
 * main.cpp —— 应用层：任务编排、IPC、看门狗、复位循环保护。
 *
 * 架构速查（详见 docs/ARCHITECTURE.md）：
 *
 *   Core 1                                   Core 0
 *   ├── rfidTask   prio6  I2C 唯一持有者      └── reportTask prio3  业务/上报
 *   └── healthTask prio2  巡检、算健康度
 *
 *   rfidTask --[CardEvent 队列,深度8]--> reportTask
 *
 * 三条铁律：
 *   1. 只有 rfidTask 碰 I2C。"单写者"直接消灭了总线竞争与优先级反转。
 *   2. 只有 reportTask 可以无限期阻塞（xQueueReceive(portMAX_DELAY) 类语义）。
 *      其余任务每个循环周期都必须回到调度器。
 *   3. 启动后不再 malloc。队列、缓冲、驱动对象全部静态分配。
 */
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include "app_config.h"
#include "app_log.h"
#include "i2c_bus.h"
#include "rfid_reader.h"

// ===========================================================================
//  事件定义：任务之间只传这个结构体，不共享任何可变全局量
// ===========================================================================
struct CardEvent {
    uint8_t  uid[10];
    uint8_t  uidLen;
    uint8_t  sak;
    bool     present;    // true = 进卡 / false = 离卡
    uint16_t seenCount;  // 本次在位期间被读到的次数
    uint32_t dwellMs;    // 在位时长
    uint32_t tsMs;       // 事件时间戳
    uint32_t seq;
};

namespace {

QueueHandle_t s_cardQueue = nullptr;
TaskHandle_t  s_hRfid = nullptr;
TaskHandle_t  s_hReport = nullptr;
TaskHandle_t  s_hHealth = nullptr;

// 业务侧累计量。
//   s_inCount / s_outCount / s_denyCount 只被 reportTask 写、healthTask 读；
//   s_dropCount 只被 rfidTask 写、healthTask 读。
// 都是"单写者 + 单读者"，且每次只做一次 uint32_t 的自增/读取 —— ESP32 上
// 这已经是单指令原子操作，不需要额外同步；而读者最多看到略旧的值，
// 对一个统计量来说完全可接受。
uint32_t s_inCount = 0;      // 进卡事件数
uint32_t s_outCount = 0;     // 离卡事件数
uint32_t s_denyCount = 0;    // 未通过白名单的次数
uint32_t s_dropCount = 0;    // 队列满而丢弃的事件数（writer: rfidTask）

constexpr uint32_t kBootMagic = 0x52464944u;  // 'RFID'

// 断连续复位计数：放在 RTC 域，普通复位不会清零（掉电才丢）。
// 用途是识别"一启动就崩"的复位循环 —— 现场最常见、也最难查的故障形态。
RTC_NOINIT_ATTR uint32_t s_bootMagic;
RTC_NOINIT_ATTR uint32_t s_bootCount;

bool s_safeMode = false;

// ------------------------------------------------------------------ 小工具
const char *resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_UNKNOWN:   return "UNKNOWN";
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SW_REBOOT";
        case ESP_RST_PANIC:     return "PANIC";      // 非法指令/断言
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";   // 任务卡死被看门狗复位
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";   // 供电跌落，天线启动瞬间最易触发
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "?";
    }
}

/** UID 转成"字节之间带空格"的可读形式，如 "27 1F BE 06"。
 *  交接给同事测试时这一点很重要：连续写法的 271FBE06 要人自己去断字节，
 *  带空格的形式肉眼一眼就能跟卡片背面/上位机对上。缓冲区至少 3*len 字节。
 */
void uidToSpaced(const uint8_t *uid, uint8_t len, char *out, size_t outSize) {
    size_t pos = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < len; ++i) {
        const int n = snprintf(out + pos, (pos < outSize) ? outSize - pos : 0,
                               (i == 0) ? "%02X" : " %02X", uid[i]);
        if (n <= 0 || static_cast<size_t>(n) >= outSize - pos) {
            break;  // 截断保护：宁可少打印一个字节，也不越界
        }
        pos += static_cast<size_t>(n);
    }
}

// ------------------------------------------------------- 业务钩子：门禁白名单
// 换成从 NVS / 后端下发的名单即可，接口不用动。
constexpr uint8_t kAllowed[][4] = {
    {0x27, 0x1F, 0xBE, 0x06},
};
bool isAllowed(const uint8_t *uid, uint8_t len) {
    if (len != 4) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kAllowed) / sizeof(kAllowed[0]); ++i) {
        if (memcmp(uid, kAllowed[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

// ===========================================================================
//  启动硬件自检（精简版）
// ===========================================================================
/**
 * 为什么生产固件也要做这件事：
 *   "接错线 / 焊盘开路 / SDA-SCL 锡桥"这三类问题的现场表现都是**读不到卡**，
 *   与"模块坏了"完全一样。启动时花不到 1 ms 把结论直接打在串口上，
 *   就能把这一整轮来回排查省掉 —— 尤其是板子交到同事手上时。
 *
 * 测量复用总线层的 checkPin()/busShorted()，与 bring-up 固件是同一份实现，
 * 判据不会漂移（这个项目已经吃过"同一逻辑两处实现"的亏）。
 *
 * 必须在 i2cbus::begin()（即 rfid::begin()）之前调用 —— 引脚此刻还没被
 * I2C 外设接管，才能当普通 GPIO 测。
 */
void printBootSelfCheck() {
    const i2cbus::PinHealth sda = i2cbus::checkPin(CFG_PIN_I2C_SDA);
    const i2cbus::PinHealth scl = i2cbus::checkPin(CFG_PIN_I2C_SCL);

    // 锡桥检查要求两根线各自先健康，否则"读回低"可以被"线上没上拉"解释，结论二义。
    const bool shorted = (sda.ok() && scl.ok())
                             ? i2cbus::busShorted(CFG_PIN_I2C_SDA, CFG_PIN_I2C_SCL)
                             : false;

    if (!sda.ok() || !scl.ok()) {
        // 逐项报出到底哪一项不合格，而不是只说一句"自检失败"。
        appLogLine("SELF", "!! I2C 引脚自检不合格 SDA(PULLUP=%d DRIVE0=%d HiZ=%d) "
                           "SCL(PULLUP=%d DRIVE0=%d HiZ=%d)",
                   sda.pullUp, sda.drive0, sda.hiZ, scl.pullUp, scl.drive0, scl.hiZ);
        appLogLine("SELF", "   判读: PULLUP=0 -> 被钉在GND(连焊/击穿) | "
                           "DRIVE0=1 -> 输出级坏 | HiZ=0 -> 线断/焊盘开路/模块没接");
    } else if (shorted) {
        appLogLine("SELF", "!! SDA 与 SCL 短路（锡桥）-> 先清理焊点，否则 I2C 不可能通");
    } else {
        appLogLine("SELF", "I2C 引脚自检通过  SDA=GPIO%d SCL=GPIO%d  无短路",
                   CFG_PIN_I2C_SDA, CFG_PIN_I2C_SCL);
    }
}

// ===========================================================================
//  采集任务（Core 1 / prio 6）—— I2C 的唯一持有者
//
//  两条互斥的执行路径：
//    A. 链路不可用（Link::DOWN / LOST）-> 指数退避恢复。
//       这条路径上"什么都不做"才是正确行为。模块没接好时以 30ms 的节奏刷总线，
//       既拿不到任何新信息，又会让日志淹没真正有用的那一行 ——
//       现场看到的"持续刷屏 + 反复自愈失败"就是这么来的。
//    B. 链路正常 -> 固定 30ms 轮询寻卡 + 在位去抖。
//
//  "链路是否可用"完全由 rfid 层判定，本任务不自己数失败次数。
// ===========================================================================

/** 退避步数 -> 等待毫秒数。250<<7 = 32000 已经超过上限，靠这里的 clamp 封顶。 */
uint32_t backoffDelayMs(uint8_t step) {
    const uint32_t v = CFG_BACKOFF_MIN_MS << step;
    return (v > CFG_BACKOFF_MAX_MS) ? CFG_BACKOFF_MAX_MS : v;
}

void rfidTask(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);

    TickType_t lastWake = xTaskGetTickCount();

    CardEvent ev = {};
    uint8_t  lastUid[10] = {};
    uint8_t  lastLen = 0;
    bool     cardOn = false;
    uint16_t seen = 0;
    uint32_t firstSeenMs = 0;
    uint32_t lastSeenMs = 0;
    uint32_t lastEmitMs = 0;
    uint32_t seq = 0;

    // ---- 恢复路径状态（指数退避）----
    rfid::Link lastLink = rfid::Link::UP;
    uint8_t  recovStep = 0;
    uint32_t lastRecovMs = 0;
    uint32_t lastStateLogMs = 0;
    uint32_t recovAttempts = 0;

    for (;;) {
        esp_task_wdt_reset();
        const uint32_t now = millis();
        const rfid::Link link = rfid::linkState();

        // -------- 状态边沿：只在这里打印，不受日志节流限制 --------
        // 复位 lastStateLogMs，使紧随其后的那句"重试失败 + 诊断结论"立刻可见。
        if (link != lastLink) {
            lastLink = link;
            lastStateLogMs = 0;
            if (link == rfid::Link::DOWN) {
                appLogLine("RFID", "读卡器缺席（从未挂载）-> 进入低速重试");
            } else if (link == rfid::Link::LOST) {
                appLogLine("RFID", "链路掉线 addr=0x%02X -> 进入自愈",
                           rfid::info().addr);
            } else {
                appLogLine("RFID", "链路恢复 addr=0x%02X chip=%s（累计尝试 %lu 次）",
                           rfid::info().addr, rfid::versionName(rfid::info().version),
                           static_cast<unsigned long>(recovAttempts));
            }
        }

        // ================= 路径 A：链路不可用 =================
        if (link != rfid::Link::UP) {
            // 掉线期间若有卡在位，必须先补一条"离卡"事件。
            // 否则业务侧会认为这张卡永远在场上，门锁状态一直悬着。
            if (cardOn) {
                cardOn = false;
                memset(&ev, 0, sizeof(ev));
                memcpy(ev.uid, lastUid, lastLen);
                ev.uidLen = lastLen;
                ev.present = false;
                ev.seenCount = seen;
                ev.dwellMs = lastSeenMs - firstSeenMs;
                ev.tsMs = now;
                ev.seq = ++seq;
                if (xQueueSend(s_cardQueue, &ev, 0) != pdTRUE) {
                    ++s_dropCount;
                }
            }
            lastLen = 0;

            if ((now - lastRecovMs) >= backoffDelayMs(recovStep)) {
                lastRecovMs = now;
                ++recovAttempts;

                if (rfid::recoverLink()) {
                    recovStep = 0;
                    recovAttempts = 0;
                    // 下一轮循环的边沿检测会打出"链路恢复"
                } else {
                    if (recovStep < CFG_BACKOFF_MAX_STEP) {
                        ++recovStep;
                    }
                    // 一条日志同时给出"试了几次 / 下次等多久 / 该查什么"。
                    // 节流到 CFG_STATE_LOG_MS，状态变化时不受限。
                    if (lastStateLogMs == 0 ||
                        (now - lastStateLogMs) >= CFG_STATE_LOG_MS) {
                        lastStateLogMs = now;
                        appLogLine("RFID", "%s 第 %lu 次恢复失败，%lums 后重试 | %s",
                                   link == rfid::Link::DOWN ? "缺席" : "掉线",
                                   static_cast<unsigned long>(recovAttempts),
                                   static_cast<unsigned long>(backoffDelayMs(recovStep)),
                                   rfid::lastDiagnosis());
                    }
                }
            }

            // 退避周期是变的，不能再用 vTaskDelayUntil 的固定节拍。
            // CFG_BACKOFF_TICK_MS 粒度的浅睡：看门狗每 50ms 被喂一次，
            // 链路恢复的发现延迟上界也就是 50ms。
            lastWake = xTaskGetTickCount();
            vTaskDelay(pdMS_TO_TICKS(CFG_BACKOFF_TICK_MS));
            continue;
        }

        // ================= 路径 B：链路正常，正常轮询 =================
        const uint8_t *uid = nullptr;
        uint8_t uidLen = 0;
        uint8_t sak = 0;
        rfid::Poll r = rfid::poll(&uid, &uidLen, &sak);

        if (r == rfid::Poll::CARD) {
            const bool same = (uidLen == lastLen) &&
                              (lastLen > 0) &&
                              (memcmp(uid, lastUid, uidLen) == 0);
            if (same) {
                ++seen;
            } else {
                memcpy(lastUid, uid, uidLen);
                lastLen = uidLen;
                seen = 1;
                firstSeenMs = now;
            }
            lastSeenMs = now;

            bool emit = false;
            if (!cardOn) {
                cardOn = true;
                emit = true;                                  // 进卡首报
            } else if (CFG_CARD_REEMIT_MS > 0 &&
                       (now - lastEmitMs) >= CFG_CARD_REEMIT_MS) {
                emit = true;                                  // 长驻卡按周期重复上报
            }

            if (emit) {
                memset(&ev, 0, sizeof(ev));
                memcpy(ev.uid, uid, uidLen);
                ev.uidLen = uidLen;
                ev.sak = sak;
                ev.present = true;
                ev.seenCount = seen;
                ev.dwellMs = now - firstSeenMs;
                ev.tsMs = now;
                ev.seq = ++seq;
                lastEmitMs = now;

                // 非阻塞投递：业务侧来不及处理就丢弃。
                // 采集任务的实时性优先级高于"一条都不能丢"。
                //
                // 只计数，**不在这里打日志**：串口写是阻塞操作，
                // 把它放在采集热路径上等于让业务侧的拥塞反过来拖累采集 ——
                // 正是这条路径最不该被拖慢。丢弃量由 healthTask 在状态行里带出来。
                if (xQueueSend(s_cardQueue, &ev, 0) != pdTRUE) {
                    ++s_dropCount;
                }
            }
        } else {
            // NO_CARD 或 LINK_DOWN（连续失败还没达到阈值）。
            // 后者不在这里打日志：真的掉线时状态机会打出"链路掉线"那一行，
            // 偶发单次失败不值得污染日志。
            if (cardOn && (now - lastSeenMs) > CFG_CARD_RELEASE_MS) {
                cardOn = false;
                memset(&ev, 0, sizeof(ev));
                memcpy(ev.uid, lastUid, lastLen);
                ev.uidLen = lastLen;
                ev.present = false;
                ev.seenCount = seen;
                ev.dwellMs = lastSeenMs - firstSeenMs;
                ev.tsMs = now;
                ev.seq = ++seq;
                if (xQueueSend(s_cardQueue, &ev, 0) != pdTRUE) {
                    ++s_dropCount;
                }
            }
        }

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CFG_RFID_POLL_MS));
    }
}

// ===========================================================================
//  业务/上报任务（Core 0 / prio 3）
//  唯一允许"等到有事件为止"的任务。所有对外动作（串口、MQTT、继电器…）
//  都收敛到这里，采集侧因此永远不被 IO 拖慢。
//
//  输出设计（面向"同事拿着板子测"）：
//    - 刷卡是主角，用 ">>>" / "<<<" 做视觉锚点，扫一眼就知道结果
//    - UID 按字节带空格，肉眼可直接和卡片/上位机对齐
//    - 一行讲完，不换行刷屏，方便截图和粘贴
// ===========================================================================
void reportTask(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);

    CardEvent ev;
    char uidStr[40];   // 10 字节 UID 带空格 = 29 字符，留足余量

    for (;;) {
        if (xQueueReceive(s_cardQueue, &ev, pdMS_TO_TICKS(CFG_REPORT_WAIT_MS)) == pdTRUE) {
            uidToSpaced(ev.uid, ev.uidLen, uidStr, sizeof(uidStr));

            if (ev.present) {
                const bool allow = isAllowed(ev.uid, ev.uidLen);
                ++s_inCount;
                if (!allow) {
                    ++s_denyCount;
                }
                appLogLine("CARD", ">>> 读卡成功  UID = %-14s sak=0x%02X  %s",
                           uidStr, ev.sak, allow ? "[放行 ALLOW]" : "[拒绝 DENY ]");
                // TODO: 这里接继电器 / MQTT 上报 / 后端校验
            } else {
                ++s_outCount;
                appLogLine("CARD", "<<< 卡片移开  UID = %-14s 在位 %lu ms  读取 %u 次",
                           uidStr, static_cast<unsigned long>(ev.dwellMs),
                           static_cast<unsigned>(ev.seenCount));
            }
        }
        esp_task_wdt_reset();
    }
}

// ===========================================================================
//  巡检任务（Core 1 / prio 2）
//  只读统计量，绝不碰 I2C。它的成本必须接近零，否则会反过来影响采集。
//
//  这个任务有两个**必须分开**的周期，混在一起会直接触发看门狗：
//    CFG_HEALTH_TICK_MS  循环周期，兼任喂狗 —— 必须远小于 CFG_TWDT_SECONDS
//    CFG_STATE_LINE_MS   状态行打印周期 —— 只影响日志，多长都行
//  早期版本两者共用一个常量，把它调大到 10 秒就会让看门狗先超时。
// ===========================================================================
void healthTask(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);

    const char *kLinkName[] = {"DOWN", "LOST", "UP"};
    uint32_t lastLineMs = 0;
    bool     firstLine = true;

    for (;;) {
        esp_task_wdt_reset();

        const uint32_t now = millis();
        // 首行不等一个完整周期：启动约 1 秒后就打一行，给操作者一个
        // "板子活着、链路是什么状态"的即时确认，之后才回到正常节流节奏。
        if (firstLine || (now - lastLineMs) >= CFG_STATE_LINE_MS) {
            firstLine = false;
            lastLineMs = now;

            const rfid::Info &ri = rfid::info();
            const rfid::Link link = rfid::linkState();

            // 状态行：一句话回答"还能不能读、读了多少、链路行不行"。
            appLogLine("STATE",
                       "运行 %lus | 进卡 %lu 离卡 %lu 拒绝 %lu 丢弃 %lu | "
                       "链路 %s  addr=0x%02X %s",
                       static_cast<unsigned long>(now / 1000),
                       static_cast<unsigned long>(s_inCount),
                       static_cast<unsigned long>(s_outCount),
                       static_cast<unsigned long>(s_denyCount),
                       static_cast<unsigned long>(s_dropCount),
                       kLinkName[static_cast<uint8_t>(link)], ri.addr,
                       link == rfid::Link::UP ? rfid::versionName(ri.version)
                                              : rfid::lastDiagnosis());

#if CFG_VERBOSE_HEALTH
            // 研发模式：每次状态行都附上全量诊断。
            const i2cbus::Diag &d = i2cbus::diag();
            appLogLine("STATE",
                       "tx=%lu ok=%lu nack=%lu timeout=%lu other=%lu rec=%lu probe=%lu heaptop=%lu",
                       static_cast<unsigned long>(d.tx),
                       static_cast<unsigned long>(d.ok),
                       static_cast<unsigned long>(d.nack),
                       static_cast<unsigned long>(d.timeout),
                       static_cast<unsigned long>(d.other),
                       static_cast<unsigned long>(d.recoveries),
                       static_cast<unsigned long>(d.linkProbes),
                       static_cast<unsigned long>(ESP.getMinFreeHeap()));
            appLogLine("STATE", "stack watermark rfid=%u report=%u health=%u (bytes left)",
                       static_cast<unsigned>(uxTaskGetStackHighWaterMark(s_hRfid)),
                       static_cast<unsigned>(uxTaskGetStackHighWaterMark(s_hReport)),
                       static_cast<unsigned>(uxTaskGetStackHighWaterMark(s_hHealth)));
#else
            // 同事测试模式：健康时保持安静，只在真的越过门限时才多打一行。
            // 这样"日志里出现 WARN"本身就是有意义的信号，不会被日常输出冲淡。
            const unsigned wRfid = uxTaskGetStackHighWaterMark(s_hRfid);
            const unsigned wReport = uxTaskGetStackHighWaterMark(s_hReport);
            const unsigned wHealth = uxTaskGetStackHighWaterMark(s_hHealth);
            const uint32_t heap = ESP.getFreeHeap();

            if (wRfid < CFG_ALERT_STACK_MIN || wReport < CFG_ALERT_STACK_MIN ||
                wHealth < CFG_ALERT_STACK_MIN) {
                appLogLine("WARN", "栈水位偏低 rfid=%u report=%u health=%u (门限 %d)，"
                                   "调大 CFG_TASK_STACK_*",
                           wRfid, wReport, wHealth, CFG_ALERT_STACK_MIN);
            }
            if (heap < CFG_ALERT_HEAP_MIN) {
                appLogLine("WARN", "剩余堆偏低 free=%lu (门限 %d)",
                           static_cast<unsigned long>(heap), CFG_ALERT_HEAP_MIN);
            }
            if (s_dropCount > 0) {
                appLogLine("WARN", "事件队列曾溢出，累计丢弃 %lu 条；"
                                   "若持续增长说明业务侧处理不过来",
                           static_cast<unsigned long>(s_dropCount));
            }
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(CFG_HEALTH_TICK_MS));
    }
}

} // namespace

// ===========================================================================
//  生命周期
// ===========================================================================
static void countBootAndLog() {
    if (s_bootMagic != kBootMagic) {
        s_bootMagic = kBootMagic;
        s_bootCount = 0;
    }
    ++s_bootCount;

    const esp_reset_reason_t rr = esp_reset_reason();
    appLogLine("BOOT", "reset_reason=%d(%s) 连续复位计数=%lu",
               static_cast<int>(rr), resetReasonName(rr),
               static_cast<unsigned long>(s_bootCount));

    if (s_bootCount >= CFG_BOOTLOOP_THRESHOLD) {
        s_safeMode = true;
        appLogLine("BOOT", "!! 疑似复位循环 -> 进入安全模式（只自检，不启动业务）");
    }
}

static void printBanner() {
    appLogRaw("\n");
    appLogRaw("=========================================================\n");
    appLogRaw(" HS-S62A-PL 读卡器   ESP32-S3   测试固件 v1.1.0\n");
    appLogRaw("=========================================================\n");
    appLogRaw(" I2C %lu kHz   SDA=GPIO%d   SCL=GPIO%d   轮询 %d ms\n",
              static_cast<unsigned long>(CFG_I2C_FREQ_HZ / 1000UL), CFG_PIN_I2C_SDA,
              CFG_PIN_I2C_SCL, CFG_RFID_POLL_MS);
    appLogRaw(" 引脚定义唯一来源: src/app_config.h\n");
    appLogRaw(" 日志通道: %s\n", appLogChannelName());
    appLogRaw("---------------------------------------------------------\n");
    appLogRaw(" 操作方法: 把卡片靠近读卡器（有效距离 0~2cm）\n");
    appLogRaw("           成功时打印 \">>> 读卡成功\"，移开时打印 \"<<< 卡片移开\"\n");
    appLogRaw("           每 %d 秒打印一行 STATE 状态；无异常时不会有多余输出\n",
              CFG_STATE_LINE_MS / 1000);
    appLogRaw("=========================================================\n");
}

void setup() {
    // 两路串口（原生 USB + 板载 USB-UART 桥）由 appLogInit() 统一打开，
    // 决策集中在 app_config.h 的 CFG_LOG_* 开关里，这里不出现任何端口名。
    appLogInit();

    printBanner();

    // 硬件自检必须在 rfid::begin() 之前 —— 引脚此刻还没被 I2C 外设接管，
    // 才能当普通 GPIO 测。这一步把"接线错/线断/锡桥"和"模块坏"区分开。
    printBootSelfCheck();
    countBootAndLog();

    // 看门狗：任务卡死 -> panic -> 自动复位，而不是无声无息地僵住。
    esp_err_t wdtErr = esp_task_wdt_init(CFG_TWDT_SECONDS, true);
    if (wdtErr != ESP_OK) {
        appLogLine("WDT", "esp_task_wdt_init=%d，沿用现有配置", static_cast<int>(wdtErr));
    }

    if (s_safeMode) {
        // 安全模式下不再拉起任何业务任务，只保留低速自检输出，
        // 让人能连上看串口把复位原因读出来。
        for (;;) {
            appLogLine("SAFE", "安全模式：heap=%lu，等待人工干预",
                       static_cast<unsigned long>(ESP.getFreeHeap()));
            delay(3000);
        }
    }

    s_cardQueue = xQueueCreate(CFG_Q_CARD_LEN, sizeof(CardEvent));
    configASSERT(s_cardQueue != nullptr);

    const bool mounted = rfid::begin();
    if (!mounted) {
        // 这里是整个启动流程里信息量最大的一行：把"现在能不能跑业务"
        // 和"该去查什么"一起说清楚，现场不用翻手册。
        appLogLine("BOOT", "读卡器未挂载，采集任务将以指数退避方式重试"
                           " (250ms -> 10s)。诊断：%s",
                   rfid::lastDiagnosis());
    } else {
        appLogLine("BOOT", "读卡器就绪  addr=0x%02X  %s", rfid::info().addr,
                   rfid::versionName(rfid::info().version));
        appLogLine("BOOT", "可以开始测试：请把卡片靠近读卡器");
    }

    // 挂载成功说明这一次启动是健康的，把复位循环计数清零。
    if (rfid::info().ready) {
        s_bootCount = 0;
    }

    xTaskCreatePinnedToCore(rfidTask, "rfid", CFG_TASK_STACK_RFID, nullptr,
                            CFG_PRIO_RFID, &s_hRfid, CFG_CORE_RFID);
    xTaskCreatePinnedToCore(reportTask, "report", CFG_TASK_STACK_REPORT, nullptr,
                            CFG_PRIO_REPORT, &s_hReport, CFG_CORE_REPORT);
    xTaskCreatePinnedToCore(healthTask, "health", CFG_TASK_STACK_HEALTH, nullptr,
                            CFG_PRIO_HEALTH, &s_hHealth, CFG_CORE_HEALTH);

    appLogLine("BOOT", "任务已启动 heap=%lu",
               static_cast<unsigned long>(ESP.getFreeHeap()));
}

void loop() {
    // 业务全部在独立任务里跑。loopTask 保留但只做最低频的巡检，
    // 绝不在这里阻塞式等待 —— 它是 Arduino 框架的宿主任务。
    // 预留位置：OTA 轮询、按键扫描、指示灯心跳。
    delay(1000);
}
