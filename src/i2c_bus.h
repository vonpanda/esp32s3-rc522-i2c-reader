/**
 * i2c_bus.h —— I2C 总线层（最底层，不依赖任何器件语义）
 *
 * 职责边界：
 *   只负责"把字节可靠地放到总线上"，以及"总线坏了怎么救回来"。
 *   不关心对端是 RC522 还是 EEPROM。
 *
 * 关键设计：
 *   1. 单写者 —— 全进程只有采集任务调用这里的收发接口，天然没有竞争，
 *      也就不需要在热路径上加锁。
 *   2. 错误分类 —— 把 Arduino 的 endTransmission() 返回值翻译成可运维的语义：
 *        2 = NACK_ADDRESS（总线正常，但该地址没人应答）
 *        5 = TIMEOUT     （事务未完成：被拉死 / 无上拉 / 未供电）
 *      这两个值指向完全不同的硬件问题，现场靠它们就能分流。
 *   3. 物理层恢复 —— 从机在传输中途被复位会一直拉低 SDA，此后所有事务都超时。
 *      标准解法是补 9 个时钟 + 一个 STOP，把它顶出死锁。
 */
#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <Arduino.h>

namespace i2cbus {

/** 一次探测/扫描的错误码分布。
 *
 *  diagnose() 只吃这个结构，绝不看 Diag 里的累计统计 —— 累计值会被
 * "不同引脚配置下的尝试"污染。踩过的坑：bring-up 的第 3 步用互换引脚扫到过
 * 一个设备，于是 Diag.ok 变成 1，此后所有"从未有人应答"的判据全部失效，
 * diagnose() 在整整 451 次 NACK 面前给出了"总线通信正常"的结论。
 * 诊断函数必须是本次探测的纯函数，这样的错误从结构上不可能再发生。
 */
struct ErrTally {
    uint16_t ok;
    uint16_t nack;
    uint16_t timeout;
    uint16_t other;

    uint16_t total() const {
        return static_cast<uint16_t>(ok + nack + timeout + other);
    }
    void add(uint8_t err);
};

/** 自初始化以来的事务统计。用于巡检任务算链路健康度。 */
struct Diag {
    uint32_t tx;         // 本层发起的事务总数（不含链路探针）
    uint32_t ok;         // endTransmission()==0
    uint32_t nack;       // ==2
    uint32_t timeout;    // ==5
    uint32_t other;      // 1/3/4
    uint32_t recoveries; // 物理层恢复次数
    uint32_t linkProbes; // 链路探针次数（单独计数，避免把健康度指标冲淡）
    uint8_t lastErr;
    uint8_t lastAddr;
};

enum class Init : uint8_t {
    OK = 0,
    BUS_REJECTED,     // Wire.begin() 返回 false
    CLOCK_REJECTED,   // Wire.setClock() 返回 false
};

/** 一根线上测到的三种状态，用于判断外部上拉是否存在、线是否被拉死。 */
struct LineDiag {
    int8_t hiZ;       // 不使能内部上下拉时的电平：1 => 存在外部上拉
    int8_t pullUp;    // 内部上拉时的电平：0 => 被强驱低 / 短路
    int8_t pullDown;  // 内部下拉时的电平：1 => 存在强外部上拉
};

/** 单根线的健康快照（四态）。回答"这个引脚到底坏没坏"，而不是靠猜。
 *
 *  判据链每一项都对应一种确定的硬件故障：
 *    pullUp == 0  -> 该脚被钉在 GND（连焊 / 击穿）
 *    drive0 == 1  -> 开漏拉不低，输出级失效，该脚不能用
 *    hiZ    == 0  -> 线上没有外部上拉可达：焊盘开路 / 走线断 / 从机没接
 *
 *  ⚠️ drive0 检不出焊盘开路：焊盘断开时，输出级拉低的仍是芯片内侧那一小段，
 *     回读照样是 0。真正能识破开路的是 hiZ —— 没有外部上拉可达，它就浮空。
 */
struct PinHealth {
    int8_t pullUp;
    int8_t pullDown;
    int8_t hiZ;
    int8_t drive0;

    /** 三态基本健康：没被钉低、能拉低、外部上拉可达。 */
    bool ok() const { return pullUp == 1 && drive0 == 0 && hiZ == 1; }
};

/** 测单根线的四态。仅在总线未启用（或已 end()）时调用。
 *  drive0 用开漏而非推挽 —— I2C 总线本就是开漏的，即使从机此刻正在驱动
 *  同一根线也不会出现电流对冲。返回前把引脚还原成 INPUT_PULLUP。
 */
PinHealth checkPin(int pin);

/** SDA / SCL 之间是否有锡桥（两根线被焊成同一个网络）。
 *  手法：一根开漏拉低、另一根保持 INPUT_PULLUP，看另一根是否被带着走；两个方向都测。
 *  返回前两根线都还原成 INPUT_PULLUP。
 *
 *  为什么必须单独测：两根线焊在一起时，**每一根单看都完全"正常"**
 *  —— 电平正常、上拉在位、能拉低。逐根线的三态测试对它完全隐形。
 *  重新焊接 / 压排线之后这是最常见的失效，而排针上 SDA/SCL 通常相邻。
 *
 *  前提：两根线各自已通过 checkPin().ok()。否则"读回低电平"可以被
 *  "这根线上压根没有上拉"解释，结论就是二义的。
 */
bool busShorted(int a, int b);

/** 初始化总线并设定时钟/超时。幂等：内部先 end() 再 begin()。 */
Init begin();

/** 指定引脚版本。用于 bring-up 阶段验证 SDA/SCL 是否接反。 */
Init begin(int sda, int scl, uint32_t hz);

/** 总线驱动当前是否处于已启用状态。重试路径据此决定要不要重新 begin()。 */
bool isUp();

int sdaPin();
int sclPin();

/** 探测单地址。返回 endTransmission() 原始码（0=成功，2=NACK，5=超时）。
 *  这个函数会累加 Diag::tx，只用于"挂载/扫描"这类需要真实数据的路径。
 */
uint8_t probe(uint8_t addr);

/** 链路探针：只问"这个地址还在不在"，1 个事务 ≈ 0.15ms。
 *  计入 Diag::linkProbes，不计入 tx —— 它不代表任何有效数据吞吐，
 *  混进 tx 会把健康度指标冲淡。
 */
uint8_t linkProbe(uint8_t addr);

/** 扫描 [CFG_RFID_SCAN_FIRST, CFG_RFID_SCAN_LAST]。
 *  @param found    输出缓冲，写满 maxFound 后只计数不再写入
 *  @param tally    可选。传入则累计本次扫描的错误码分布
 *  @return 实际应答的设备数量（可能大于 maxFound）
 */
uint8_t scan(uint8_t *found, uint8_t maxFound, ErrTally *tally = nullptr);

/** 物理层恢复：结束总线 -> GPIO 位翻转补时钟 + STOP -> 重新初始化。
 *  @return 恢复后对 addr 探测成功返回 true
 */
bool recover(uint8_t addr);

/** 量测某根线的物理状态。仅在总线未被 I2C 外设占用时有效。 */
LineDiag probeLine(int pin);

Diag &diag();
void resetDiag();

/** 把 endTransmission() 返回值翻译成人话。 */
const char *wireErrName(uint8_t err);

/** 根据"本次探测的错误码分布"给出"下一步该查什么"的一句话结论。
 *  纯函数：不改任何状态、不读任何累计量。返回静态常量字符串。
 */
const char *diagnose(const ErrTally &t);

} // namespace i2cbus

#endif // I2C_BUS_H
