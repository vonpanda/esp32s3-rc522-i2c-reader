#include "rfid_reader.h"

#include <new>
#include <string.h>

#include "YFROBOTRFIDI2C.h"
#include "app_config.h"
#include "app_log.h"
#include "i2c_bus.h"

namespace rfid {

namespace {

// 静态存储 + placement new。
// 两个原因：
//   1. 真实地址要扫描后才知道（0x28 还是 0x2F），但驱动对象必须在编译期定下来；
//   2. RTOS 任务运行期不允许 malloc —— 用栈上/静态存储反复构造，零堆分配。
alignas(YFROBOTRFID) uint8_t s_storage[sizeof(YFROBOTRFID)];
YFROBOTRFID *s_rc = nullptr;

Info s_info = {};

uint8_t s_uidBuf[10] = {};
uint8_t s_uidLen = 0;
uint8_t s_sak = 0;

// 上一次"寄存器级深检"的时刻。浅检每轮都做，深检按 CFG_LINK_DEEPCHECK_MS 节流。
uint32_t s_lastDeepMs = 0;

// 最近一次挂载/恢复失败的结论。静态字符串，生命周期无限长。
const char *s_lastDiag = "尚未做过挂载尝试。";

/** VersionReg 的合法取值集合。取这些值之外（尤其 0x00 / 0xFF）即视为链路死。 */
bool versionValid(uint8_t v) {
    switch (v) {
        case 0x82:  // "new" MFRC522
        case 0x88:  // FM17522 clone
        case 0x90:  // MFRC522 v0.0
        case 0x91:  // MFRC522 v1.0
        case 0x92:  // SI522A @0x28
        case 0xB2:  // SI522A @0x2F
            return true;
        default:
            return false;
    }
}

/** 深检：一次寄存器回读。代价 2 个 I2C 事务（写地址+读数据）。
 *  这是唯一能识破"地址还在 ACK，但芯片内部已卡死"的手段 ——
 *  裸地址探测对这种情况一无所知，会一直报告"链路正常、场上无卡"。
 */
bool linkAliveDeep() {
    if (s_rc == nullptr) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        if (versionValid(s_rc->ReadVersion())) {
            return true;
        }
        delay(1);
    }
    return false;
}

/** 链路探针（分两档）。
 *  浅检 = 裸地址探测，1 个事务 ≈ 0.15ms，每轮都做；
 *  深检 = VersionReg 回读，按周期做。
 */
bool linkProbeTiered() {
    if (s_rc == nullptr) {
        return false;
    }
    s_info.linkProbes++;

    const uint32_t now = millis();
    if ((now - s_lastDeepMs) >= CFG_LINK_DEEPCHECK_MS) {
        s_lastDeepMs = now;
        return linkAliveDeep();
    }
    return i2cbus::linkProbe(s_info.addr) == 0;
}

bool attach(uint8_t addr) {
    s_rc = new (s_storage) YFROBOTRFID(addr);
    s_info.addr = addr;

    // 挂载必须用深检。浅检只能证明"地址有人 ACK"，
    // 万一 ACK 它的是别的器件（地址撞车）或芯片半死，后面 PCD_Init 会拿到一堆垃圾。
    s_info.linkProbes++;
    s_lastDeepMs = millis();
    if (!linkAliveDeep()) {
        s_rc = nullptr;
        return false;
    }
    s_info.version = s_rc->ReadVersion();

    // 到这里芯片已确认应答，可以放心做完整初始化。
    // 库内 PCD_Reset() 的无界等待已在 lib/YFROBOTRFIDI2C 里打过补丁，有界。
    s_rc->PCD_Init();

    // 缩短 RC522 内部定时器：默认 0x03E8(1000) * 25us = 25ms。
    // 无卡时这 25ms 会白白阻塞读卡任务，直接决定轮询周期的下限。
    s_rc->PCD_WriteRegister(YFROBOTRFID::TReloadRegH,
                            static_cast<byte>((CFG_RC522_TRELOAD >> 8) & 0xFF));
    s_rc->PCD_WriteRegister(YFROBOTRFID::TReloadRegL,
                            static_cast<byte>(CFG_RC522_TRELOAD & 0xFF));

    s_info.ready = true;
    s_info.everMounted = true;
    s_info.failStreak = 0;
    return true;
}

/** 把一个地址从"探测"到"挂载完成"跑一遍。任何一步失败都返回 false。
 *  探测结果同时记进 tally —— 诊断必须基于"这一次到底发生了什么"。
 */
bool tryMount(uint8_t addr, const char *tag, i2cbus::ErrTally &tally) {
    const uint8_t err = i2cbus::probe(addr);
    tally.add(err);
    if (err != 0) {
        appLogLine("RFID", "%s: 0x%02X 无应答 (endTransmission=%u %s)", tag, addr, err,
                   i2cbus::wireErrName(err));
        return false;
    }
    if (!attach(addr)) {
        appLogLine("RFID", "%s: 0x%02X 有应答但寄存器回读非法 -> 芯片未就绪或欠压", tag,
                   addr);
        return false;
    }
    s_info.initOk++;
    appLogLine("RFID", "%s: 挂载成功 addr=0x%02X chip=%s", tag, s_info.addr,
               versionName(s_info.version));
    return true;
}

} // namespace

const char *versionName(uint8_t v) {
    switch (v) {
        case 0x82: return "MFRC522 (new, 0x82)";
        case 0x88: return "FM17522 clone (0x88)";
        case 0x90: return "MFRC522 v0.0 (0x90)";
        case 0x91: return "MFRC522 v1.0 (0x91)";
        case 0x92: return "SI522A @0x28 (0x92)";
        case 0xB2: return "SI522A @0x2F (0xB2)";
        default:   return "unknown";
    }
}

const char *lastDiagnosis() { return s_lastDiag; }

bool begin() {
    i2cbus::Init ir = i2cbus::begin();
    if (ir != i2cbus::Init::OK) {
        appLogLine("RFID", "I2C 总线初始化失败 (%s)",
                   ir == i2cbus::Init::BUS_REJECTED ? "Wire.begin 拒绝" : "setClock 拒绝");
        s_lastDiag = "I2C 驱动无法启用，与本层无关的问题。";
        return false;
    }

    // 本次挂载尝试的全部证据收集在这里。诊断只看它，不看累计统计。
    i2cbus::ErrTally tally = {};

    // 顺序固定为：手册默认地址 -> 变体地址 -> 全总线扫描。
    // 全总线扫描是最后手段，因为它是全场最贵的动作。
    if (tryMount(CFG_RFID_ADDR_PRIMARY, "上电", tally)) {
        return true;
    }
    if (tryMount(CFG_RFID_ADDR_ALT, "变体地址", tally)) {
        return true;
    }

    uint8_t found[8] = {};
    const uint8_t n = i2cbus::scan(found, sizeof(found), &tally);
    if (n == 0) {
        s_lastDiag = i2cbus::diagnose(tally);
        appLogLine("RFID", "全总线扫描无任何应答 (%u 个地址) -> %s", tally.total(),
                   s_lastDiag);
        return false;
    }

    appLogLine("RFID", "扫描到 %u 个设备: %02X %02X %02X %02X", n, found[0], found[1],
               found[2], found[3]);
    // 第一个应答的地址不一定是读卡器，所以再走一次寄存器回读确认。
    if (tryMount(found[0], "扫描", tally)) {
        return true;
    }

    s_lastDiag = "总线上有器件应答但不是 HS-S62A-PL：地址撞车或模块型号不对。";
    appLogLine("RFID", "扫描到的设备均非 HS-S62A-PL 读卡器");
    return false;
}

bool retry() {
    // 走到这里说明总线之前可能被 end() 过（物理层恢复、或上一次 begin 失败），
    // 先确保驱动处于启用状态。
    if (!i2cbus::isUp()) {
        if (i2cbus::begin() != i2cbus::Init::OK) {
            s_lastDiag = "I2C 驱动无法启用，与本层无关的问题。";
            return false;
        }
    }

    // 静默版本：重试路径每几百 ms 就会跑一次，日志留给上层按退避曲线节流输出。
    // 但结论要留下来，上层那句"重试失败 | <结论>"就是从这里取的。
    i2cbus::ErrTally tally = {};

    const uint8_t e1 = i2cbus::probe(CFG_RFID_ADDR_PRIMARY);
    tally.add(e1);
    if (e1 == 0 && attach(CFG_RFID_ADDR_PRIMARY)) {
        s_info.initOk++;
        return true;
    }

    const uint8_t e2 = i2cbus::probe(CFG_RFID_ADDR_ALT);
    tally.add(e2);
    if (e2 == 0 && attach(CFG_RFID_ADDR_ALT)) {
        s_info.initOk++;
        return true;
    }

    s_lastDiag = i2cbus::diagnose(tally);
    return false;
}

bool heal() {
    if (!s_info.everMounted) {
        // 从来没挂上过，就不存在"已知地址"可供恢复。退回廉价重试用。
        return retry();
    }

    s_info.healCount++;
    s_info.ready = false;

    // 物理层解卡优先：模块在传输中途掉电/复位会把 SDA 拉死，
    // 此后所有事务都是 TIMEOUT，看起来像"模块坏了"，其实补 9 个时钟就好。
    i2cbus::recover(s_info.addr);

    for (int round = 0; round < CFG_MAX_HEAL_ROUNDS; ++round) {
        if (attach(s_info.addr)) {
            s_info.initOk++;
            appLogLine("RFID", "自愈成功 (第 %d 轮) chip=%s heal=%lu", round + 1,
                       versionName(s_info.version),
                       static_cast<unsigned long>(s_info.healCount));
            return true;
        }
        i2cbus::recover(s_info.addr);
        delay(CFG_HEAL_BACKOFF_MS);
    }

    return false;
}

bool recoverLink() {
    switch (linkState()) {
        case Link::UP:   return true;
        case Link::LOST: return heal();
        case Link::DOWN: return retry();
    }
    return false;
}

Link linkState() {
    if (s_info.ready) {
        return Link::UP;
    }
    return s_info.everMounted ? Link::LOST : Link::DOWN;
}

Poll poll(const uint8_t **uid, uint8_t *uidLen, uint8_t *sak) {
    s_info.polls++;

    if (!s_info.ready || s_rc == nullptr) {
        return Poll::LINK_DOWN;
    }

    // 用 WUPA 而不是 REQA：WUPA 能唤醒 IDLE 与 HALT 两种状态的卡，
    // 这样配合 HaltA 才能判断出"卡还在不在"。REQA 对已 HALT 的卡永远不应答。
    byte atqa[2] = {0, 0};
    byte atqaSize = sizeof(atqa);
    byte st = s_rc->PICC_WakeupA(atqa, &atqaSize);

    if (st == YFROBOTRFID::STATUS_OK || st == YFROBOTRFID::STATUS_COLLISION) {
        if (s_rc->PICC_ReadCardSerial()) {
            s_uidLen = (s_rc->uid.size > sizeof(s_uidBuf)) ? sizeof(s_uidBuf)
                                                          : s_rc->uid.size;
            memcpy(s_uidBuf, s_rc->uid.uidByte, s_uidLen);
            s_sak = s_rc->uid.sak;

            s_rc->PICC_HaltA();      // 回到 HALT，下一轮靠 WUPA 叫醒
            s_rc->PCD_StopCrypto1(); // 退出加密状态，否则下一轮通信起不来

            s_info.cards++;
            s_info.failStreak = 0;
            if (uid) *uid = s_uidBuf;
            if (uidLen) *uidLen = s_uidLen;
            if (sak) *sak = s_sak;
            return Poll::CARD;
        }
    }

    // 走到这里有两种可能：场上真没卡，或者模块掉线了。
    // 用链路探针把它们分开 —— 这是本层最有价值的一条信息。
    if (linkProbeTiered()) {
        s_info.failStreak = 0;
        return Poll::NO_CARD;
    }

    s_info.linkDowns++;
    if (++s_info.failStreak >= CFG_LINK_FAIL_STREAK) {
        // 连续失败达标：才把链路判为掉线并把判定权交给上层。
        s_info.ready = false;
    }
    return Poll::LINK_DOWN;
}

const Info &info() { return s_info; }

} // namespace rfid
