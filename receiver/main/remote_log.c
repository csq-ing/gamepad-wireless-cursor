#include "remote_log.h"

#if CONFIG_REMOTE_LOG_ENABLE

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "espnow_handler.h"

static vprintf_like_t s_original_vprintf = NULL;

/* ESP32-S2 is single-core, so a plain bool is sufficient as recursion guard. */
static bool s_in_remote_log = false;

#define REMOTE_LOG_BUF_SIZE 240

static int remote_vprintf(const char *fmt, va_list args)
{
    int ret = 0;

    if (s_original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    if (s_in_remote_log) {
        return ret;
    }
    s_in_remote_log = true;

    char buf[REMOTE_LOG_BUF_SIZE];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        size_t send_len = (len < (int)sizeof(buf)) ? (size_t)len
                                                    : sizeof(buf) - 1;
        espnow_send_log(buf, send_len);
    }

    s_in_remote_log = false;
    return ret;
}

esp_err_t remote_log_init(void)
{
    s_original_vprintf = esp_log_set_vprintf(remote_vprintf);
    return ESP_OK;
}

#endif /* CONFIG_REMOTE_LOG_ENABLE */
