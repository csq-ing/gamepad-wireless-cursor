#include "motor_control.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Motor_DataProcess.h"
#include "ze300.h"

#define MOTOR_TASK_PERIOD_MS 10
#define MOTOR_TASK_STACK_SIZE 4096
#define MOTOR_TASK_PRIORITY 5
#define MOTOR_DEBUG_TASK_STACK_SIZE 3072
#define MOTOR_DEBUG_TASK_PRIORITY 2

#define MOTOR_TORQUE_HARD_LIMIT_MA 2000
#define MOTOR_DEFAULT_SHAKE_MA 800
#define MOTOR_DEFAULT_SHAKE_HALF_PERIOD_MS 80
#define MOTOR_DEFAULT_SHAKE_CYCLES 3
#define MOTOR_DEFAULT_DAMPING_LIMIT_MA 1200
#define MOTOR_DEFAULT_DAMPING_KP 6.0f
#define MOTOR_DEFAULT_SPEED_ALPHA 0.35f
#define MOTOR_DEFAULT_TORQUE_ALPHA 0.15f
#define MOTOR_DEFAULT_SPEED_DEADBAND_RPM 3.0f
#define MOTOR_STATUS_PERIOD_MS 500

typedef struct {
    motor_control_state_t state;
    TickType_t state_started_at;
    int32_t shake_ma;
    uint32_t shake_half_period_ms;
    uint8_t shake_cycles;
    int32_t damping_limit_ma;
    float damping_kp;
    float speed_filter_alpha;
    float torque_filter_alpha;
    float speed_deadband_rpm;
} motor_runtime_config_t;

static portMUX_TYPE s_motor_lock = portMUX_INITIALIZER_UNLOCKED;
static motor_runtime_config_t s_cfg = {
    .state = MOTOR_STATE_NORMAL,
    .state_started_at = 0,
    .shake_ma = MOTOR_DEFAULT_SHAKE_MA,
    .shake_half_period_ms = MOTOR_DEFAULT_SHAKE_HALF_PERIOD_MS,
    .shake_cycles = MOTOR_DEFAULT_SHAKE_CYCLES,
    .damping_limit_ma = MOTOR_DEFAULT_DAMPING_LIMIT_MA,
    .damping_kp = MOTOR_DEFAULT_DAMPING_KP,
    .speed_filter_alpha = MOTOR_DEFAULT_SPEED_ALPHA,
    .torque_filter_alpha = MOTOR_DEFAULT_TORQUE_ALPHA,
    .speed_deadband_rpm = MOTOR_DEFAULT_SPEED_DEADBAND_RPM,
};

static bool s_task_started = false;
static uint8_t s_last_vibration_left = 0;
static uint8_t s_last_vibration_right = 0;
static float s_filtered_speed_rpm = 0.0f;
static float s_filtered_torque_ma = 0.0f;
static TickType_t s_last_status_at = 0;

static const char *state_name(motor_control_state_t state)
{
    switch (state) {
    case MOTOR_STATE_NORMAL:
        return "NORMAL";
    case MOTOR_STATE_BITE_SHAKE:
        return "BITE_SHAKE";
    case MOTOR_STATE_FISH_DAMPING:
        return "FISH_DAMPING";
    default:
        return "UNKNOWN";
    }
}

static motor_runtime_config_t copy_config(void)
{
    motor_runtime_config_t cfg;

    portENTER_CRITICAL(&s_motor_lock);
    cfg = s_cfg;
    portEXIT_CRITICAL(&s_motor_lock);

    return cfg;
}

static void update_state(motor_control_state_t state,
                         int32_t shake_ma,
                         uint32_t shake_half_period_ms,
                         uint8_t shake_cycles,
                         int32_t damping_limit_ma,
                         float damping_kp)
{
    const int32_t safe_shake_ma =
        motor_dp_sanitize_torque_limit(shake_ma, MOTOR_TORQUE_HARD_LIMIT_MA);
    const uint32_t safe_half_ms =
        motor_dp_clamp_u32(shake_half_period_ms, 20, 1000);
    const uint8_t safe_cycles =
        (uint8_t)motor_dp_clamp_u32(shake_cycles, 1, 20);
    const int32_t safe_damping_limit =
        motor_dp_sanitize_torque_limit(damping_limit_ma, MOTOR_TORQUE_HARD_LIMIT_MA);
    const float safe_kp = motor_dp_clamp_float(damping_kp, 0.0f, 200.0f);

    portENTER_CRITICAL(&s_motor_lock);
    s_cfg.state = state;
    s_cfg.state_started_at = xTaskGetTickCount();
    s_cfg.shake_ma = safe_shake_ma;
    s_cfg.shake_half_period_ms = safe_half_ms;
    s_cfg.shake_cycles = safe_cycles;
    s_cfg.damping_limit_ma = safe_damping_limit;
    s_cfg.damping_kp = safe_kp;
    portEXIT_CRITICAL(&s_motor_lock);

    printf("[motor] state -> %s shake=%ldmA half=%lums cycles=%u damping_limit=%ldmA kp=%.2f\n",
           state_name(state),
           (long)safe_shake_ma,
           (unsigned long)safe_half_ms,
           (unsigned)safe_cycles,
           (long)safe_damping_limit,
           (double)safe_kp);
}

static void update_filter_params(float speed_alpha, float torque_alpha, float deadband_rpm)
{
    const float safe_speed_alpha = motor_dp_clamp_float(speed_alpha, 0.01f, 1.0f);
    const float safe_torque_alpha = motor_dp_clamp_float(torque_alpha, 0.01f, 1.0f);
    const float safe_deadband = motor_dp_clamp_float(deadband_rpm, 0.0f, 50.0f);

    portENTER_CRITICAL(&s_motor_lock);
    s_cfg.speed_filter_alpha = safe_speed_alpha;
    s_cfg.torque_filter_alpha = safe_torque_alpha;
    s_cfg.speed_deadband_rpm = safe_deadband;
    portEXIT_CRITICAL(&s_motor_lock);

    printf("[motor] filters speed_alpha=%.2f torque_alpha=%.2f deadband=%.1frpm\n",
           (double)safe_speed_alpha,
           (double)safe_torque_alpha,
           (double)safe_deadband);
}

static void reset_damping_runtime(void)
{
    s_filtered_speed_rpm = 0.0f;
    s_filtered_torque_ma = 0.0f;
}

static bool status_due(TickType_t now)
{
    if (s_last_status_at == 0 ||
        (now - s_last_status_at) >= pdMS_TO_TICKS(MOTOR_STATUS_PERIOD_MS)) {
        s_last_status_at = now;
        return true;
    }
    return false;
}

static void print_motor_status(const motor_runtime_config_t *cfg,
                               const ze300_state_t *state,
                               float filtered_speed_rpm,
                               int32_t torque_cmd_ma)
{
    printf("[motor] state=%s angle=%7.1f deg speed=%+7.2f rpm filt=%+7.2f rpm q_current=%+7.3f A torque_cmd=%+6ld mA temp=%5.1f C\n",
           state_name(cfg->state),
           (double)state->angle_deg,
           (double)state->speed_rpm,
           (double)filtered_speed_rpm,
           (double)state->q_current_a,
           (long)torque_cmd_ma,
           (double)state->temperature_c);
}

static void print_periodic_motor_status(const motor_runtime_config_t *cfg,
                                        TickType_t now,
                                        int32_t torque_cmd_ma)
{
    ze300_state_t state;

    if (!status_due(now)) {
        return;
    }

    esp_err_t err = ze300_read_state(&state);
    if (err != ESP_OK) {
        printf("[motor] state=%s telemetry read failed: %s\n",
               state_name(cfg->state),
               esp_err_to_name(err));
        return;
    }

    print_motor_status(cfg, &state, s_filtered_speed_rpm, torque_cmd_ma);
}

static void run_bite_shake(const motor_runtime_config_t *cfg, TickType_t now)
{
    const uint32_t elapsed_ms =
        (uint32_t)((now - cfg->state_started_at) * portTICK_PERIOD_MS);
    const uint32_t phase = elapsed_ms / cfg->shake_half_period_ms;
    const uint32_t total_phases = (uint32_t)cfg->shake_cycles * 2U;

    if (phase >= total_phases) {
        (void)ze300_set_torque(0);
        update_state(MOTOR_STATE_FISH_DAMPING,
                     cfg->shake_ma,
                     cfg->shake_half_period_ms,
                     cfg->shake_cycles,
                     cfg->damping_limit_ma,
                     cfg->damping_kp);
        return;
    }

    const int32_t torque_ma = (phase & 1U) ? -cfg->shake_ma : cfg->shake_ma;
    (void)ze300_set_torque(torque_ma);
    print_periodic_motor_status(cfg, now, torque_ma);
}

static void run_fish_damping(const motor_runtime_config_t *cfg, TickType_t now)
{
    static int read_fail_count = 0;
    ze300_state_t state;

    esp_err_t err = ze300_read_state(&state);
    if (err != ESP_OK) {
        read_fail_count++;
        if ((read_fail_count % 10) == 1) {
            printf("[motor] damping read failed: %s\n", esp_err_to_name(err));
        }
        (void)ze300_set_torque(0);
        return;
    }
    read_fail_count = 0;

    const float raw_speed = state.speed_rpm;
    s_filtered_speed_rpm =
        cfg->speed_filter_alpha * raw_speed +
        (1.0f - cfg->speed_filter_alpha) * s_filtered_speed_rpm;

    float torque_raw = 0.0f;
    if (motor_dp_abs_float(s_filtered_speed_rpm) >= cfg->speed_deadband_rpm) {
        const float speed_error = 0.0f - s_filtered_speed_rpm;
        torque_raw = cfg->damping_kp * speed_error;
        torque_raw = motor_dp_clamp_float(torque_raw,
                                          (float)-cfg->damping_limit_ma,
                                          (float)cfg->damping_limit_ma);
    } else {
        s_filtered_torque_ma = 0.0f;
    }

    s_filtered_torque_ma =
        cfg->torque_filter_alpha * torque_raw +
        (1.0f - cfg->torque_filter_alpha) * s_filtered_torque_ma;

    const int32_t torque_ma = motor_dp_clamp_i32((int32_t)s_filtered_torque_ma,
                                                -cfg->damping_limit_ma,
                                                cfg->damping_limit_ma);
    (void)ze300_set_torque(torque_ma);

    if (status_due(now)) {
        print_motor_status(cfg, &state, s_filtered_speed_rpm, torque_ma);
    }
}

static void motor_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    motor_control_state_t previous_state = MOTOR_STATE_NORMAL;
    TickType_t previous_started_at = 0;

    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        const motor_runtime_config_t cfg = copy_config();

        if (cfg.state != previous_state || cfg.state_started_at != previous_started_at) {
            previous_state = cfg.state;
            previous_started_at = cfg.state_started_at;
            reset_damping_runtime();
            s_last_status_at = 0;
            if (cfg.state == MOTOR_STATE_NORMAL) {
                (void)ze300_set_torque(0);
            }
        }

        switch (cfg.state) {
        case MOTOR_STATE_NORMAL:
            print_periodic_motor_status(&cfg, now, 0);
            break;
        case MOTOR_STATE_BITE_SHAKE:
            run_bite_shake(&cfg, now);
            break;
        case MOTOR_STATE_FISH_DAMPING:
            run_fish_damping(&cfg, now);
            break;
        default:
            update_state(MOTOR_STATE_NORMAL,
                         cfg.shake_ma,
                         cfg.shake_half_period_ms,
                         cfg.shake_cycles,
                         cfg.damping_limit_ma,
                         cfg.damping_kp);
            break;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MOTOR_TASK_PERIOD_MS));
    }
}

static void print_debug_help(void)
{
    printf("\n[motor dbg] commands:\n");
    printf("  n | normal\n");
    printf("  s [cycles]\n");
    printf("  d [limit_ma]\n");
    printf("  f [speed_alpha] [deadband_rpm]\n");
    printf("  cfg\n");
    printf("Examples: s 3, d 1000, f 0.35 3\n\n");
}

static void print_debug_config(void)
{
    const motor_runtime_config_t cfg = copy_config();
    uint8_t left;
    uint8_t right;

    portENTER_CRITICAL(&s_motor_lock);
    left = s_last_vibration_left;
    right = s_last_vibration_right;
    portEXIT_CRITICAL(&s_motor_lock);

    printf("[motor cfg] state=%s shake=%ldmA half=%lums cycles=%u damping_limit=%ldmA kp=%.2f speed_alpha=%.2f torque_alpha=%.2f deadband=%.1frpm vibration=(%u,%u)\n",
           state_name(cfg.state),
           (long)cfg.shake_ma,
           (unsigned long)cfg.shake_half_period_ms,
           (unsigned)cfg.shake_cycles,
           (long)cfg.damping_limit_ma,
           (double)cfg.damping_kp,
           (double)cfg.speed_filter_alpha,
           (double)cfg.torque_filter_alpha,
           (double)cfg.speed_deadband_rpm,
           (unsigned)left,
           (unsigned)right);
}

static void handle_debug_command(char *line)
{
    char cmd[16] = {0};
    unsigned long value = 0;
    float f1 = 0.0f;
    float f2 = 0.0f;

    if (sscanf(line, "%15s", cmd) != 1) {
        return;
    }

    if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_debug_help();
        return;
    }

    if (strcmp(cmd, "cfg") == 0) {
        print_debug_config();
        return;
    }

    const motor_runtime_config_t cfg = copy_config();

    if (strcmp(cmd, "n") == 0 || strcmp(cmd, "normal") == 0 || strcmp(cmd, "stop") == 0) {
        update_state(MOTOR_STATE_NORMAL,
                     cfg.shake_ma,
                     cfg.shake_half_period_ms,
                     cfg.shake_cycles,
                     cfg.damping_limit_ma,
                     cfg.damping_kp);
        printf("Write OK normal\n");
        return;
    }

    if (strcmp(cmd, "s") == 0 || strcmp(cmd, "shake") == 0) {
        const int n = sscanf(line, "%*s %lu", &value);
        const uint8_t cycles = (n >= 1) ? (uint8_t)value : cfg.shake_cycles;

        update_state(MOTOR_STATE_BITE_SHAKE,
                     cfg.shake_ma,
                     cfg.shake_half_period_ms,
                     cycles,
                     cfg.damping_limit_ma,
                     cfg.damping_kp);
        printf("Write OK shake cycles=%u\n", (unsigned)cycles);
        return;
    }

    if (strcmp(cmd, "d") == 0 || strcmp(cmd, "damp") == 0 || strcmp(cmd, "damping") == 0) {
        const int n = sscanf(line, "%*s %lu", &value);
        const int32_t limit_ma = (n >= 1) ? (int32_t)value : cfg.damping_limit_ma;

        update_state(MOTOR_STATE_FISH_DAMPING,
                     cfg.shake_ma,
                     cfg.shake_half_period_ms,
                     cfg.shake_cycles,
                     limit_ma,
                     cfg.damping_kp);
        printf("Write OK damping limit=%ldmA\n", (long)limit_ma);
        return;
    }

    if (strcmp(cmd, "f") == 0 || strcmp(cmd, "filter") == 0) {
        const int n = sscanf(line, "%*s %f %f", &f1, &f2);
        const float speed_alpha = (n >= 1) ? f1 : cfg.speed_filter_alpha;
        const float deadband = (n >= 2) ? f2 : cfg.speed_deadband_rpm;

        update_filter_params(speed_alpha, cfg.torque_filter_alpha, deadband);
        printf("Write OK filter\n");
        return;
    }

    printf("[motor dbg] unknown command: %s\n", cmd);
    print_debug_help();
}

static void motor_debug_console_task(void *arg)
{
    (void)arg;
    char line[96];

    print_debug_help();

    for (;;) {
        if (fgets(line, sizeof(line), stdin) != NULL) {
            handle_debug_command(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

esp_err_t motor_control_init(void)
{
    esp_err_t err = ze300_init();
    if (err != ESP_OK) {
        printf("[motor] ze300 init failed: %s\n", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    ze300_clear_fault();
    ze300_set_torque(0);

    if (!s_task_started) {
        BaseType_t rc = xTaskCreatePinnedToCore(motor_task,
                                                "motor_state",
                                                MOTOR_TASK_STACK_SIZE,
                                                NULL,
                                                MOTOR_TASK_PRIORITY,
                                                NULL,
                                                0);
        if (rc != pdPASS) {
            return ESP_FAIL;
        }

        rc = xTaskCreatePinnedToCore(motor_debug_console_task,
                                     "motor_debug",
                                     MOTOR_DEBUG_TASK_STACK_SIZE,
                                     NULL,
                                     MOTOR_DEBUG_TASK_PRIORITY,
                                     NULL,
                                     0);
        if (rc != pdPASS) {
            return ESP_FAIL;
        }

        s_task_started = true;
    }

    printf("[motor] ZE300 initialized; fishing motor state machine ready\n");
    return ESP_OK;
}

esp_err_t motor_control_apply_vibration(uint8_t left, uint8_t right)
{
    portENTER_CRITICAL(&s_motor_lock);
    s_last_vibration_left = left;
    s_last_vibration_right = right;
    portEXIT_CRITICAL(&s_motor_lock);

    return ESP_OK;
}
