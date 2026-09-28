/**
 * rfid_reader.h —— 读卡器设备层
 *
 * 职责边界：
 *   把 HS-S62A-PL (MFRC522 / SI522A) 的寄存器操作封装成"挂载 / 寻卡 / 恢复"
 *   三个业务语义动作，并把厂商库的阻塞行为收敛到可控范围内。
 *   同时它是"链路是否可用"的唯一判定方 —— 上层不该自己数失败次数。
 *
 * 为什么不能直接照抄厂商例程：
 *   1. 厂商例程在 setup() 里裸调 PCD_Init()。库内 PCD_Reset() 是无界忙等，
 *      模块上电时序异常时（该库 README 自己承认的已知问题）会永久卡死。
 *   2. 厂商例程不区分"场上没卡"和"模块掉线"，两者都表现为读卡失败。
 *      现场无法据此判断该换卡还是该修模块。
 *   3. 厂商例程用 REQA 寻卡。REQA 只能唤醒 IDLE 态的卡，被 HALT 过的卡
 *      永远不应答 —— 导致"卡拔走了没有"根本判断不出来。
 *      这里改用 WUPA（可唤醒 IDLE 与 HALT），配合 HaltA 才能做出在位检测。
 */
#ifndef RFID_READER_H
#define RFID_READER_H

#include <Arduino.h>

namespace rfid {

enum class Poll : uint8_t {
    CARD,       // 读到卡，uid/uidLen/sak 有效
    NO_CARD,    // 链路正常，但场上没有卡
    LINK_DOWN,  // 本轮判活失败。注意：连续失败未达阈值前 ready 仍为 true，
                // 上层应把它当"这轮没读到"处理，不要立刻大动干戈。
};

/** 链路三态。上层的恢复动作完全由它决定，不需要自己推断。 */
enum class Link : uint8_t {
    DOWN,   // 从未挂载成功 —— 地址未知，模块缺席（接线/供电还没弄好）
    LOST,   // 曾挂载成功，现在掉线 —— 已知地址，走物理层解卡 + 重初始化
    UP,     // 已挂载且链路健康
};

struct Info {
    uint8_t  addr;        // 当前使用的 I2C 地址（从未挂载过则为 0）
    uint8_t  version;     // VersionReg(0x37) 回读值
    bool     ready;       // 链路健康
    bool     everMounted; // 是否曾经成功挂载过（决定 Link::DOWN 还是 Link::LOST）
    uint32_t initOk;      // 累计成功挂载次数
    uint32_t healCount;   // 自愈次数
    uint32_t polls;       // 寻卡总次数
    uint32_t cards;       // 读到卡的次数
    uint32_t linkDowns;   // 链路判活失败次数
    uint32_t linkProbes;  // 链路探针次数（浅检 + 深检）
    uint8_t  failStreak;  // 当前连续失败次数
};

/** 上电挂载：找地址 -> 判活 -> 完整初始化 -> 必要时自愈。
 *  这是"第一次接触模块"的完整流程，包含最贵的全总线扫描。
 *  @return 挂载成功返回 true；失败不阻塞、不卡死，由调用方决定重试节奏。
 */
bool begin();

/** 快速重新挂载：只试两个已知地址（0x28 / 0x2F），不做全总线扫描。
 *  专供"已确认模块缺席"后的周期重试使用 —— 代价恒定上界（≤2 个事务），
 *  这样退避曲线才有意义。反之，如果每次重试都全扫 112 个地址，
 *  线路超时的情况下单次重试就要吃掉 2 秒以上。
 */
bool retry();

/** 单次寻卡。非阻塞语义，但内部最长可能耗时约 CFG_RC522_TRELOAD 对应的
 *  RC522 定时器周期（默认 ≈12.8ms），这是本模块最大的单次阻塞点。
 *
 *  内部维护"连续失败计数"：只有连续失败达到 CFG_LINK_FAIL_STREAK 次才把
 *  ready 置 false。单次失败只返回 LINK_DOWN，链路仍算在线。
 */
Poll poll(const uint8_t **uid, uint8_t *uidLen, uint8_t *sak);

/** 物理层恢复 + 重新挂载。要求模块曾经挂载成功过（否则没有已知地址）。
 *  @return 恢复后链路可用返回 true。
 */
bool heal();

/** 链路恢复总入口：按当前的 Link 状态自动选动作。
 *    DOWN -> retry()（廉价，快速试两个已知地址）
 *    LOST -> heal() （物理层解卡 + 完整重初始化）
 *    UP   -> 直接返回 true（无需恢复）
 */
bool recoverLink();

Link linkState();
const Info &info();

/** 最近一次挂载/恢复失败的原因结论（一句话，给现场人看）。
 *  由 begin()/retry() 在失败时写入，供日志引用。
 *  返回静态生命周期字符串，可以安全地长期持有。
 */
const char *lastDiagnosis();

/** 把 VersionReg 翻译成可读的芯片型号（含地址变体提示）。 */
const char *versionName(uint8_t version);

} // namespace rfid

#endif // RFID_READER_H
