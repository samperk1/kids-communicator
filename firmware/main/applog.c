#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "applog.h"

static const char *TAG = "app";

static char        s_lines[APPLOG_LINES][APPLOG_COLS + 1];
static int         s_head;          /* next slot to write */
static int         s_count;
static unsigned    s_seq;
static SemaphoreHandle_t s_lock;

static void push(const char *text)
{
    strlcpy(s_lines[s_head], text, sizeof(s_lines[0]));
    s_head = (s_head + 1) % APPLOG_LINES;
    if (s_count < APPLOG_LINES) s_count++;
    s_seq++;
}

/* 🚨 CREATE THE MUTEX ONCE, UP FRONT — LAZY INIT HERE WAS A RACE.
 *
 * applog() is called from the LVGL task, the voice task, the wt task AND the
 * system event task. `if (!s_lock) s_lock = xSemaphoreCreateMutex()` inside the
 * function means two of them can pass that test at the same time, create two
 * mutexes, and then serialise against different objects — so the ring buffer is
 * no longer protected at all. Call this from app_main() before anything logs. */
void applog_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

void applog(const char *fmt, ...)
{
    /* 🚨 THIS BUFFER IS ON THE STACK OF WHOEVER CALLS US, AND ONE OF THOSE IS
     * THE SYSTEM EVENT TASK, WHICH HAS 2304 BYTES TOTAL.
     *
     * At 512 bytes plus vsnprintf's own frame plus ESP_LOGI formatting on top,
     * logging from an event handler overflowed that stack and corrupted
     * FreeRTOS's structures. It died in xQueueGiveMutexRecursive inside
     * esp_event_loop_run with EXCVADDR 0x29 — a near-null read in the kernel,
     * nowhere near the actual culprit. Measured 2026-08-15 after repeated WiFi
     * connect/disconnect cycles, each of which logs from that task.
     * The stack is also raised in sdkconfig.defaults; both together, because
     * either alone leaves this too close to the edge. */
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Everything also goes to the serial log — the panel is an addition, not a
     * replacement, and a crash loop is only ever readable over UART. */
    ESP_LOGI(TAG, "%s", buf);

    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Wrap rather than truncate. A 400-character assistant reply is the whole
     * reason this panel exists; cutting it at column 34 would hide the answer. */
    size_t len = strlen(buf);
    if (len <= APPLOG_COLS) {
        push(buf);
    } else {
        for (size_t i = 0; i < len; i += APPLOG_COLS) {
            char chunk[APPLOG_COLS + 1];
            snprintf(chunk, sizeof(chunk), "%.*s", APPLOG_COLS, buf + i);
            push(chunk);
        }
    }

    if (s_lock) xSemaphoreGive(s_lock);
}

int applog_count(void) { return s_count; }
unsigned applog_seq(void) { return s_seq; }

const char *applog_line(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    /* Oldest first. When the ring has wrapped, the oldest sits at s_head. */
    int start = (s_count == APPLOG_LINES) ? s_head : 0;
    return s_lines[(start + i) % APPLOG_LINES];
}
