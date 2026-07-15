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

enum {
    MOTOR_TASK_PERIOD_MS = 20,
    MOTOR_TASK_STACK_SIZE = 4096,
    MOTOR_TASK_PRIORITY = 5,
    MOTOR_DEBUG_TASK_STACK_SIZE = 3072,
    MOTOR_DEBUG_TASK_PRIORITY = 2,
};

typedef struct {
    motor_control_state_t state;
    TickType_t state_started_at;
    int32_t shake_ma;
    uint8_t shake_cycles;
    int32_t damping_limit_ma;
    float speed_filter_alpha;
    float speed_deadband_rpm;
} motor_runtime_config_t;

static portMUX_TYPE s_motor_lock = portMUX_INITIALIZER_UNLOCKED;
static motor_runtime_config_t s_cfg = {
    .state = MOTOR_STATE_NORMAL,
    .state_started_at = 0,
    .shake_ma = 800,
    .shake_cycles = 3,
    .damping_limit_ma = 1200,
    .speed_filter_alpha = 0.35f,
    .speed_deadband_rpm = 1.0f,
};

ze300_state_t state;

static bool s_task_started = false;
static uint8_t s_last_vibration_left = 0;
static uint8_t s_last_vibration_right = 0;
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



int32_t run_bite_shake(const motor_runtime_config_t *cfg)
{
    static uint32_t systick=0;
    static int32_t torque_ma = cfg->shake_ma;
    systick++;
    if(systick==systick*cfg->shake_cycles)
    {
        cfg->state=MOTOR_STATE_FISH_DAMPING;
        systick=0;
    }
    else if(systick%cfg->shake_cycles==1)
    {
        torque_ma=-torque_ma;
    }

    return torque_ma;
}

int32_t run_fish_damping(const motor_runtime_config_t *cfg)
{
    static float s_filtered_speed_rpm = 0.0f;

    uint8_t Kd=50;

    s_filtered_speed_rpm =
        cfg->speed_filter_alpha * state.speed_rpm +
        (1.0f - cfg->speed_filter_alpha) * s_filtered_speed_rpm;

    int32_t torque_ma = -Kd * s_filtered_speed_rpm;

    if (torque_ma > cfg->damping_limit_ma) 
    
        torque_ma = cfg->damping_limit_ma;
        
    else if (torque_ma < -cfg->damping_limit_ma) 

        torque_ma = -cfg->damping_limit_ma;

    return torque_ma;

}


static void motor_task(void *arg)
{
    int32_t torque_ma = 0;
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    motor_control_state_t last_state = MOTOR_STATE_NORMAL;
    TickType_t last_state_started_at = 0;

    ze300_read_state(&state);

    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        const motor_runtime_config_t cfg = copy_config();

        if (cfg.state != last_state || cfg.state_started_at != last_state_started_at) {
            last_state = cfg.state;
            last_state_started_at = cfg.state_started_at;
            s_filtered_speed_rpm = 0.0f;
            s_last_status_at = 0;
            if (cfg.state == MOTOR_STATE_NORMAL) {
                ze300_set_torque(0);
            }
        }

        switch (cfg.state) 
        {
            case MOTOR_STATE_NORMAL: 
                torque_ma = 0;
                break;
            case MOTOR_STATE_BITE_SHAKE:
                torque_ma=run_bite_shake(&cfg);
                break;
            case MOTOR_STATE_FISH_DAMPING:
                torque_ma=run_fish_damping(&cfg);
                break;
            default:
                torque_ma = 0;
                break;
        }

        ze300_set_torque(torque_ma);
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
    printf("Examples: s 3, d 1000, f 0.35 1\n\n");
}


static void handle_debug_command(char *line)
{
    char *saveptr = NULL;
    char *cmd = strtok_r(line, " \t\r\n", &saveptr);
    const motor_runtime_config_t cfg = copy_config();

    if (cmd == NULL) {
        return;
    }

    if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_debug_help();
        return;
    }

    if (strcmp(cmd, "n") == 0 || strcmp(cmd, "normal") == 0 || strcmp(cmd, "stop") == 0) {

        printf("Write OK normal\n");
        return;
    }

    if (strcmp(cmd, "s") == 0 || strcmp(cmd, "shake") == 0) 
    {
        char *arg = strtok_r(NULL, " \t\r\n", &saveptr);

        cfg.shake_cycles =
            (arg != NULL) ? (uint8_t)strtoul(arg, NULL, 10) : cfg.shake_cycles;

        printf("Write OK shake cycles=%u\n", cfg.shake_cycles);
        return;
    }

    if (strcmp(cmd, "d") == 0 || strcmp(cmd, "damp") == 0 || strcmp(cmd, "damping") == 0) 
    {
        char *arg = strtok_r(NULL, " \t\r\n", &saveptr);
        const int32_t cfg.damping_limit_ma =
            (arg != NULL) ? (int32_t)strtol(arg, NULL, 10) : cfg.damping_limit_ma;

        printf("Write OK damping limit=%ldmA\n", cfg.damping_limit_ma);
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
    const esp_err_t err = ze300_init();
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
