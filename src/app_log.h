/**
 * app_log.h —— 全局唯一的串口输出入口。
 *
 * 为什么不让各任务直接 Serial.print：
 *   1. Arduino-ESP32 的 Serial(HWCDC) 不是线程安全的，多任务并发 printf 会互相
 *      踩缓冲区，输出串行、错行甚至丢字。这里用一把互斥锁把格式化 + 写出绑成
 *      原子操作。互斥锁带优先级继承，低优先级任务日志时不会把高优先级的读卡
 *      任务长时间挡住。
 *   2. 输出通道的决策必须只有一处。本板有两路串口（原生 USB 的 HWCDC、
 *      板载 USB-UART 桥的 UART0），现场最怕"固件打到了一个同事没接的口上"，
 *      所以由 appLogInit() 统一按 app_config.h 的开关决定，并同时写两路。
 */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <Arduino.h>

/** 初始化日志：建互斥锁 + 按 app_config.h 的开关打开两路串口。
 *  必须在创建任何任务之前、在 setup() 里调用一次。
 *  调用后两路串口的打印内容完全一致。
 */
void appLogInit();

/** 带 "[时间][标签] " 前缀并自动换行。 */
void appLogLine(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/** 不带前缀、不自动换行（用于打印多行横幅）。 */
void appLogRaw(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/** 当前实际打开的通道，用于启动横幅自报家门（"日志打到了哪个口"）。 */
const char *appLogChannelName();

#endif // APP_LOG_H
