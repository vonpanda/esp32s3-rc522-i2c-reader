# YFROBOTRFIDI2C 本地补丁记录（v1.0.3）

> 上游原始代码保留在 `docs/vendor/YFROBOTRFIDI2C-1.0.3-original/`。
> **升级库时必须以本表为检查清单重新打补丁**，否则下面的问题会全部回归。

补丁动机：原库在通信异常时存在**无界忙等**。在带看门狗的高优先级任务里，
这会表现成"卡死 → 看门狗复位 → 又卡死"的复位循环，现场极难定位。

---

## PATCH-1 `PCD_Reset()` —— 无界 `while` 死循环（严重）

原代码：

```cpp
while (PCD_ReadRegister(CommandReg) & (1<<4)) {
    // PCD still restarting
}
```

问题：模块未应答时 `PCD_ReadRegister()` 内部 `Wire.requestFrom()` 失败，
`Wire.read()` 返回 `-1`，赋给 `byte` 即 `0xFF`；`0xFF & (1<<4)` 恒为真。
→ **一旦模块上电时序异常，固件永久卡死在这里。**

这与该库自己 README 记录的"重新上电异常初始化无法正常通讯"是同一个坑。

修改：改为有界重试（20 次 × 5 ms = 上限 100 ms），超时后返回，由上层判定链路故障。

## PATCH-2 `PCD_CalculateCRC()` —— 加入墙钟上限

原循环 `word i = 5000; while(1){...}`，每次迭代含 1 次 I2C 读。
总线"半死"（能 ACK 但 IRQ 不置位）时最坏可拖到数十秒。
修改：在保留原计数上限的同时，增加 100 ms 墙钟上限。

## PATCH-3 `PCD_CommunicateWithPICC()` —— 加入墙钟上限

同上，原上限 `i = 2000`，总线半死时同样可能拖到数十秒。
修改：增加 100 ms 墙钟上限。

---

## 未修改但必须知道的行为（由上层封装兜底）

| 行为 | 说明 | 上层对策 |
|---|---|---|
| `PICC_RequestA/WakeupA` 无卡时阻塞 | 芯片内部定时器耗尽才返回 `STATUS_TIMEOUT`，默认 25 ms | 上层把 `TReloadReg` 改为 512（≈12.8 ms） |
| `PCD_WriteRegister` 忽略 `endTransmission()` 返回值 | 写失败静默丢弃 | 上层用 `VersionReg(0x37)` 回读做链路判活 |
| 库不使用 `Wire.setClock` 之外的并发保护 | 非线程安全 | 全进程只有 `rfidTask` 一个 I2C 持有者 |
| `PCD_PerformSelfTest()` 对 SI522A 无效 | 厂商注释已说明 | 不使用该函数，改用 `VersionReg` 判活 |
