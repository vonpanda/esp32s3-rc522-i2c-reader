#include "app_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"

namespace {
SemaphoreHandle_t s_mutex = nullptr;
char s_buf[256];

inline void lock() {
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

inline void unlock() {
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

/**
 * 把同一段字节写到所有已启用的通道。
 *
 * 两路各自独立，互不阻塞：USB CDC 那一路已经设了写超时，主机不在时
 * 最多等 CFG_LOG_TX_TIMEOUT_MS 就丢弃；UART0 那一路是硬件 UART，
 * 有发送 FIFO + 驱动缓冲，115200 下一行日志根本排得下。
 */
inline void emit(const char *s, size_t len) {
    if (len == 0) {
        return;
    }
    const uint8_t *p = reinterpret_cast<const uint8_t *>(s);
#if CFG_LOG_USB_CDC
    Serial.write(p, len);
#endif
#if CFG_LOG_UART0
    Serial0.write(p, len);
#endif
}
} // namespace

const char *appLogChannelName() {
#if CFG_LOG_USB_CDC && CFG_LOG_UART0
    return "USB + UART0(TX43/RX44)";
#elif CFG_LOG_USB_CDC
    return "USB only";
#elif CFG_LOG_UART0
    return "UART0 only (TX43/RX44)";
#else
    return "NONE";
#endif
}

void appLogInit() {
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }

#if CFG_LOG_UART0
    // setTxBufferSize 必须在第一次 begin() 之前调用才生效，否则会被忽略。
    Serial0.setTxBufferSize(CFG_LOG_UART0_TX_BUF);
    Serial0.begin(CFG_SERIAL_BAUD);
#endif

#if CFG_LOG_USB_CDC
    // 顺序注意：先 begin，再设超时。HWCDC::begin(0) 会自动阻塞等待主机枚举，
    // 这里给个有限的 baud 参数即可，真正的"等待枚举"由下面的 settle 延时完成。
    Serial.begin(CFG_SERIAL_BAUD);
    Serial.setTxTimeoutMs(CFG_LOG_TX_TIMEOUT_MS);
#endif

    // 等 USB CDC 枚举完成，否则最前面几行日志会丢在主机还没准备好的时候。
    // 这是启动期一次性开销，不在任何任务的循环里。
#if CFG_LOG_USB_CDC
    delay(400);
#endif
}

void appLogRaw(const char *fmt, ...) {
    lock();
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_buf, sizeof(s_buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        unlock();
        return;
    }
    emit(s_buf, strnlen(s_buf, sizeof(s_buf)));
    unlock();
}

void appLogLine(const char *tag, const char *fmt, ...) {
    lock();

    int n = snprintf(s_buf, sizeof(s_buf), "[%8lu][%-6s] ",
                     static_cast<unsigned long>(millis()), tag);
    if (n < 0) {
        n = 0;
    }
    const size_t cap = sizeof(s_buf) - 1;
    size_t off = (static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : cap;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_buf + off, sizeof(s_buf) - off, fmt, ap);
    va_end(ap);

    size_t len = strnlen(s_buf, cap);
    if (len < cap) {
        s_buf[len++] = '\n';
        s_buf[len] = '\0';
    }
    emit(s_buf, len);
    unlock();
}
