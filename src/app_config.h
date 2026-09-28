/**
 * app_config.h —— 全部可调参数集中在此处。
 *
 * 约定：其余源文件里禁止出现裸魔数（引脚、超时、优先级、栈大小）。
 * 现场调参只改这一个文件，改完重新编译即可，不会漏改。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

// ------------------------------------------------------------------ 串口
#define CFG_SERIAL_BAUD                 115200

// ---- 日志输出通道 --------------------------------------------------------
// 本板有两路串口，两路同时打开，谁接哪个口都能看到（交接给同事时最省事）：
//   原生 USB 口      -> Serial  (HWCDC)     设备名 /dev/cu.usbmodem*
//   板载 USB-UART 桥 -> Serial0 (UART0)     设备名 /dev/cu.usbserial*
//                                          引脚 TX=GPIO43  RX=GPIO44
// 两者打印内容完全一致。关掉其中一路可省约 1.5 KB flash。
#define CFG_LOG_USB_CDC                 1
#define CFG_LOG_UART0                   1

// USB CDC 在"主机没在读"时的单次写超时（ms）。
// 这一项必须有上限：USB CDC 的写是同步等主机的，拔掉 USB 线之后
// 没有超时的话一次日志就能把调用任务整段阻塞住 —— USB CDC 最经典的坑。
#define CFG_LOG_TX_TIMEOUT_MS           20

// UART0 的发送缓冲（字节）。最长日志行约 200 字节，留 2 倍余量，
// 让 115200 下的 write 尽量不阻塞。**必须在第一次 begin() 之前设置才生效**。
#define CFG_LOG_UART0_TX_BUF            512

// --------------------------------------------------------------- I2C 总线
// 【引脚映射的真源在这里，改这一个文件即可，其余文件禁止出现裸引脚号】
//
// 现场实测定版的接法（2026-09-28，重新焊接后）：
//     ESP32 GPIO18  ->  模块 SDA      =>  CFG_PIN_I2C_SDA = 18
//     ESP32 GPIO17  ->  模块 SCL      =>  CFG_PIN_I2C_SCL = 17
//
// ⚠️ 这不是 ESP32 的惯例接法（惯例是 GPIO5/GPIO4 或 8/9）。
//    将来做 PCB / 压排线时，丝印必须按上表标注，
//    否则会重新踩"接反 -> 全总线 NACK"这个坑。
//
// ESP32-S3-DevKitC-1 v1.1 上的可用性核对（官方 J1 排针表）：
//   J1 pin10 = GPIO17 = RTC_GPIO17 / U1TXD / ADC2_CH6   —— 板载未占用
//   J1 pin11 = GPIO18 = RTC_GPIO18 / U1RXD / ADC2_CH7   —— 板载未占用
//   UART1 本工程不用（日志与烧录走 UART0 = GPIO43/44），故可安全征用。
//   必须避开的：GPIO0/3/45/46（strapping）、GPIO19/20（原生 USB）、
//   GPIO38（v1.1 RGB LED）、GPIO39~42（JTAG）、GPIO43/44（UART0）。
//   GPIO35~37 在 Octal PSRAM 型号上被内部占用（本工程 N8R2 为 Quad，未占用）。
#define CFG_PIN_I2C_SDA                 18
#define CFG_PIN_I2C_SCL                 17
// HS-S62A-PL 手册标称 I2C 最高 400kHz。默认 100kHz 是"弱上拉 + 长排线"下的稳妥值；
// 实测稳定后可上调到 400000 提升吞吐。
#define CFG_I2C_FREQ_HZ                 100000UL
// 单次 I2C 事务的软件超时（ms）。手册默认 50ms 太长，会拖住 30ms 的读卡周期。
#define CFG_I2C_TIMEOUT_MS              20

// ------------------------------------------------------------- 读卡器地址
// 手册标称默认 0x28；但厂商库同时收录了 SI522A-2F 变体（VersionReg=0xB2，地址 0x2F）。
// 因此上电时两个地址都试，最后兜底做全总线扫描。
#define CFG_RFID_ADDR_PRIMARY           0x28
#define CFG_RFID_ADDR_ALT               0x2F
#define CFG_RFID_SCAN_FIRST             0x08
#define CFG_RFID_SCAN_LAST              0x77

// ----------------------------------------------------------- 读卡状态机
// 轮询周期。必须大于"无卡时单次寻卡的阻塞时长"，否则任务会挤在一起。
#define CFG_RFID_POLL_MS                30
// RC522 内部定时器重载值：t = 25us * N。默认 0x03E8(1000)=25ms，
// 无卡时这段时间白白阻塞读卡任务。改成 512 ≈ 12.8ms。
// 若现场偶发漏读卡，先把这里调回 1000。
#define CFG_RC522_TRELOAD               512
// 卡离开判定：连续多久没应答才算"卡走了"（去抖，防抖动误报）
#define CFG_CARD_RELEASE_MS             500
// 同一张卡长驻时的重复上报间隔（0 = 只在进卡时上报一次）
#define CFG_CARD_REEMIT_MS              2000

// --------------------------------------------------------------- 自愈策略
// 连续多少次链路判活失败才把链路判定为"掉线"。
// 单次失败不足以说明问题（排线动一下、电源纹波、EMI 都可能造成偶发 NACK），
// 连续失败才动干戈，避免自愈动作本身变成新的抖动源。
#define CFG_LINK_FAIL_STREAK            5
// 链路判活的"寄存器级深检"周期。地址探测（1 个事务 ≈ 0.15ms）每轮都做；
// 寄存器回读贵 4 倍（2 个事务 ≈ 0.6ms），按周期做。
// 深检专门用来抓"地址还在但芯片内部卡死"——浅检看不见这种故障。
#define CFG_LINK_DEEPCHECK_MS           500
// 一次自愈里最多重挂载几轮。每轮之间隔 CFG_HEAL_BACKOFF_MS。
#define CFG_MAX_HEAL_ROUNDS             3
#define CFG_HEAL_BACKOFF_MS             100

// ------------------------------------------------ 链路不可用时的指数退避
// 读卡器缺席（接线还没弄好）是正常状态，绝不是刷屏的理由：
// 绝不能以 30ms 的节奏刷总线，也不能每 150ms 打印一行"自愈失败"。
// 退避序列：250 → 500 → 1000 → 2000 → 4000 → 8000 → 10000(封顶) ms。
#define CFG_BACKOFF_MIN_MS              250
#define CFG_BACKOFF_MAX_MS              10000
#define CFG_BACKOFF_MAX_STEP            7
// 退避等待期间"浅睡"的粒度。这个值决定两件事：
//   1. 任务看门狗被喂食的最大间隔（必须远小于 CFG_TWDT_SECONDS）；
//   2. 链路恢复被发现的延迟上界。
// 取 50ms：WDT 余量充足，恢复延迟对门禁场景完全够用，CPU 占用 ≈ 0。
#define CFG_BACKOFF_TICK_MS             50
// 同一条"链路不可用"日志的最小重复间隔，避免刷屏。
// 状态发生"变化"时（缺席 <-> 掉线 <-> 恢复）不受此限制，一定立即打印。
#define CFG_STATE_LOG_MS                5000

// -------------------------------------------------------------- 任务配置
// 队列深度：按"最快连续刷卡的极端情况"取值，满了就丢，绝不阻塞采集任务。
#define CFG_Q_CARD_LEN                  8

#define CFG_TASK_STACK_RFID             4096
#define CFG_TASK_STACK_REPORT           6144
// 实测 health 任务峰值占用约 1.7KB（三条长格式串的 vsnprintf 很吃栈），
// 4096 留出 2.4KB 余量。
#define CFG_TASK_STACK_HEALTH           4096

// 优先级仅需满足：采集 > 业务 > 巡检。WiFi/BLE 协议栈跑在 18~23，比这里都高。
#define CFG_PRIO_RFID                   6
#define CFG_PRIO_REPORT                 3
#define CFG_PRIO_HEALTH                 2

// I2C 是采集任务的独占资源，必须和 WiFi/BLE 协议栈分核，避免被其长时间抢占。
#define CFG_CORE_RFID                   1
#define CFG_CORE_REPORT                 0
#define CFG_CORE_HEALTH                 1

// ---- 巡检节奏（三个周期必须分开，否则会互相拖累）--------------------------
// TICK 是巡检任务的循环周期，它同时兼任**喂看门狗**。
// 必须远小于 CFG_TWDT_SECONDS，否则任务自己就会被看门狗复位。
#define CFG_HEALTH_TICK_MS              1000
// 状态行的打印周期。只影响"打不打日志"，不影响喂狗。
// 交接给同事测试：10000（状态行不盖住刷卡输出）
// 研发阶段想看实时水位：调成 1000
#define CFG_STATE_LINE_MS               10000
// 1 = 每次状态行附上栈水位 / 堆水位 / 总线错误码直方图（研发用）
// 0 = 只在异常时才追加告警行（同事测试用，健康时保持安静）
#define CFG_VERBOSE_HEALTH              0
// 业务任务的等待粒度。只影响"事件日志与巡检日志的相对时序"，与采集周期无关。
#define CFG_REPORT_WAIT_MS              1000
// 告警门限：栈水位低于此值、或堆低于此值，就无视节流立刻打告警行。
#define CFG_ALERT_STACK_MIN             1024
#define CFG_ALERT_HEAP_MIN              20000

// --------------------------------------------------------------- 看门狗
#define CFG_TWDT_SECONDS                5
// 连续复位达到该次数则进入安全模式（只自检、不跑业务），打破复位循环
#define CFG_BOOTLOOP_THRESHOLD          8

#endif // APP_CONFIG_H
