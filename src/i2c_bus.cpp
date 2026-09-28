#include "i2c_bus.h"

#include <Wire.h>
#include <string.h>

#include "app_config.h"

namespace i2cbus {

namespace {

Diag s_diag = {};
bool s_up = false;
int  s_sda = CFG_PIN_I2C_SDA;
int  s_scl = CFG_PIN_I2C_SCL;

void account(uint8_t addr, uint8_t err) {
    s_diag.tx++;
    s_diag.lastErr = err;
    s_diag.lastAddr = addr;
    switch (err) {
        case 0:  s_diag.ok++;      break;
        case 2:  s_diag.nack++;    break;
        case 5:  s_diag.timeout++; break;
        default: s_diag.other++;   break;
    }
}

/** 结束总线占用，把两根线释放成普通 GPIO。 */
void releaseBus() {
    if (s_up) {
        Wire.end();
        s_up = false;
    }
    pinMode(s_sda, INPUT_PULLUP);
    pinMode(s_scl, INPUT_PULLUP);
}

} // namespace

const char *wireErrName(uint8_t err) {
    switch (err) {
        case 0:  return "OK";
        case 1:  return "TX_BUFFER_TOO_LONG";
        case 2:  return "NACK_ADDRESS";   // 总线通，但该地址无人应答
        case 3:  return "NACK_DATA";
        case 4:  return "OTHER_ERROR";
        case 5:  return "TIMEOUT";        // 事务未完成：被拉死/无上拉/未供电
        default: return "UNKNOWN";
    }
}

void ErrTally::add(uint8_t err) {
    switch (err) {
        case 0:  ok++;      break;
        case 2:  nack++;    break;
        case 5:  timeout++; break;
        default: other++;   break;
    }
}

const char *diagnose(const ErrTally &t) {
    if (t.total() == 0) {
        return "本次未做任何探测，无从判断。";
    }

    // TIMEOUT 压倒一切：它说明事务根本没能在总线上跑完，属于物理层问题。
    // 此时 NACK 的计数再少也没有意义 —— 总线都已经不成形了。
    if (t.timeout > 0 && t.ok == 0) {
        return "事务超时(TIMEOUT=5)：SDA/SCL 被拉死、无外部上拉、或模块未供电。"
               "先量模块 V-GND 电压与两条线的空载电平，不要动固件。";
    }

    // 全 NACK + 零 TIMEOUT + 零 OK —— 现场最容易被误读的一种组合。
    // 它说明总线电气完全正常（上拉在、边沿干净、没有线被拉死），
    // 但没有任何器件 ACK。而"接反 SDA/SCL"给出的正是这个组合：
    // 模块看到的是两个方向都反了的信号，永远解析不出合法起始条件，
    // 于是永不 ACK；但波形、电平、上拉全都"看起来正常"。
    // 只有引脚互换扫描才能识破它 —— 所以这里把它排在第一嫌疑。
    if (t.ok == 0 && t.nack == t.total()) {
        return "总线电气正常但无任何地址应答：首选怀疑 SDA/SCL 接反"
               "（接反时电平、上拉、波形全部正常，错误码清一色 NACK）；"
               "其次才是模块未供电、模块损坏、或地址不在扫描区间。";
    }

    if (t.ok == 0) {
        return "错误码混杂(既有 NACK 又有 TIMEOUT)：总线时序处于临界状态，"
               "典型原因是上拉太弱或走线过长。建议 SDA/SCL 各加 4.7k 上拉，"
               "或把 I2C 速率降到 50kHz 复测。";
    }

    return "总线上有器件应答，问题在器件级（地址 / 寄存器 / 初始化），不在总线。";
}

Init begin(int sda, int scl, uint32_t hz) {
    if (s_up) {
        Wire.end();
        s_up = false;
    }
    s_sda = sda;
    s_scl = scl;

    if (!Wire.begin(s_sda, s_scl, hz)) {
        return Init::BUS_REJECTED;
    }
    s_up = true;

    if (!Wire.setClock(hz)) {
        // 时钟设不上：不要当作致命错误继续跑，否则实际速率与预期不符，
        // 时序余量的计算全部失效。
        return Init::CLOCK_REJECTED;
    }
    Wire.setTimeOut(CFG_I2C_TIMEOUT_MS);
    return Init::OK;
}

Init begin() { return begin(CFG_PIN_I2C_SDA, CFG_PIN_I2C_SCL, CFG_I2C_FREQ_HZ); }

int sdaPin() { return s_sda; }
int sclPin() { return s_scl; }

bool isUp() { return s_up; }

uint8_t probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    account(addr, err);
    return err;
}

uint8_t linkProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    s_diag.linkProbes++;
    s_diag.lastErr = err;
    s_diag.lastAddr = addr;
    return err;
}

uint8_t scan(uint8_t *found, uint8_t maxFound, ErrTally *tally) {
    uint8_t n = 0;
    for (uint16_t addr = CFG_RFID_SCAN_FIRST; addr <= CFG_RFID_SCAN_LAST; ++addr) {
        const uint8_t e = probe(static_cast<uint8_t>(addr));
        if (tally != nullptr) {
            tally->add(e);
        }
        if (e == 0) {
            if (n < maxFound) {
                found[n] = static_cast<uint8_t>(addr);
            }
            n++;
        }
    }
    return n;
}

bool recover(uint8_t addr) {
    releaseBus();
    delayMicroseconds(10);

    // SDA 被从机拉死 -> 补最多 9 个时钟，让它把剩下的位发完并松开
    if (digitalRead(s_sda) == LOW) {
        pinMode(s_scl, OUTPUT_OPEN_DRAIN);
        digitalWrite(s_scl, HIGH);

        for (int i = 0; i < 9 && digitalRead(s_sda) == LOW; ++i) {
            digitalWrite(s_scl, LOW);
            delayMicroseconds(5);
            digitalWrite(s_scl, HIGH);
            delayMicroseconds(5);
        }

        // 补一个 STOP 条件：SCL 保持高时，SDA 由低跳到高
        pinMode(s_sda, OUTPUT_OPEN_DRAIN);
        digitalWrite(s_sda, LOW);
        delayMicroseconds(5);
        digitalWrite(s_scl, HIGH);
        delayMicroseconds(5);
        digitalWrite(s_sda, HIGH);
        delayMicroseconds(5);

        pinMode(s_sda, INPUT_PULLUP);
    }

    Init r = begin(s_sda, s_scl, CFG_I2C_FREQ_HZ);
    s_diag.recoveries++;
    if (r != Init::OK) {
        return false;
    }
    return probe(addr) == 0;
}

LineDiag probeLine(int pin) {
    LineDiag d = {};

    pinMode(pin, INPUT);
    delayMicroseconds(50);
    d.hiZ = static_cast<int8_t>(digitalRead(pin) ? 1 : 0);

    pinMode(pin, INPUT_PULLUP);
    delayMicroseconds(50);
    d.pullUp = static_cast<int8_t>(digitalRead(pin) ? 1 : 0);

    pinMode(pin, INPUT_PULLDOWN);
    delayMicroseconds(50);
    d.pullDown = static_cast<int8_t>(digitalRead(pin) ? 1 : 0);

    pinMode(pin, INPUT_PULLUP);
    return d;
}

PinHealth checkPin(int pin) {
    const LineDiag d = probeLine(pin);

    PinHealth h = {};
    h.pullUp = d.pullUp;
    h.pullDown = d.pullDown;
    h.hiZ = d.hiZ;

    // 输出侧：开漏拉低，看拉不拉得动。
    // 用开漏而不是推挽 —— 从机可能正在驱动同一根线，推挽会电流对冲。
    pinMode(pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(pin, LOW);
    delayMicroseconds(50);
    h.drive0 = static_cast<int8_t>(digitalRead(pin) ? 1 : 0);

    digitalWrite(pin, HIGH);       // 开漏释放 = 高阻
    pinMode(pin, INPUT_PULLUP);    // 还原成安全态
    return h;
}

bool busShorted(int a, int b) {
    // 方向 1：a 拉低，看 b 是否被带着走
    pinMode(a, OUTPUT_OPEN_DRAIN);
    digitalWrite(a, LOW);
    pinMode(b, INPUT_PULLUP);
    delayMicroseconds(200);
    const bool aPullsB = (digitalRead(b) == LOW);
    digitalWrite(a, HIGH);
    pinMode(a, INPUT_PULLUP);

    // 方向 2：b 拉低，看 a 是否被带着走。
    // 两个方向都要测：单方向能查到"直接短接"，但方向性失效（如某个引脚对地漏电）
    // 只在一个方向显现。
    pinMode(b, OUTPUT_OPEN_DRAIN);
    digitalWrite(b, LOW);
    pinMode(a, INPUT_PULLUP);
    delayMicroseconds(200);
    const bool bPullsA = (digitalRead(a) == LOW);
    digitalWrite(b, HIGH);
    pinMode(b, INPUT_PULLUP);

    return aPullsB || bPullsA;
}

Diag &diag() { return s_diag; }

void resetDiag() { memset(&s_diag, 0, sizeof(s_diag)); }

} // namespace i2cbus
