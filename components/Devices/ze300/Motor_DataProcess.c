#include "Motor_DataProcess.h"

static const float DAMPING_STOP_RATIO = 0.5f;
static const float DAMPING_RAW_OPPOSITE_RATIO = 2.0f;

int32_t motor_dp_clamp_i32(int32_t value, int32_t min_value, int32_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

uint32_t motor_dp_clamp_u32(uint32_t value, uint32_t min_value, uint32_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

float motor_dp_clamp_float(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

float motor_dp_abs_float(float value)
{
    return (value < 0.0f) ? -value : value;
}

float motor_dp_smoothstep(float value)
{
    const float x = motor_dp_clamp_float(value, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

int motor_dp_speed_sign(float speed, float deadband)
{
    if (speed > deadband) {
        return 1;
    }
    if (speed < -deadband) {
        return -1;
    }
    return 0;
}

float motor_dp_wrap_angle_error_deg(float target_deg, float current_deg)
{
    float error = target_deg - current_deg;

    while (error > 180.0f) {
        error -= 360.0f;
    }
    while (error < -180.0f) {
        error += 360.0f;
    }
    return error;
}

int32_t motor_dp_sanitize_torque_limit(int32_t value, int32_t hard_limit_ma)
{
    const int32_t positive = (value < 0) ? -value : value;
    return motor_dp_clamp_i32(positive, 0, hard_limit_ma);
}

int motor_dp_stable_motion_direction(float raw_speed, float filtered_speed, float deadband)
{
    const int filtered_direction = motor_dp_speed_sign(filtered_speed, deadband);
    const int strong_raw_direction =
        motor_dp_speed_sign(raw_speed, deadband * DAMPING_RAW_OPPOSITE_RATIO);

    if (filtered_direction == 0) {
        return 0;
    }
    if (strong_raw_direction != 0 && strong_raw_direction != filtered_direction) {
        return 0;
    }
    return filtered_direction;
}

bool motor_dp_damping_should_stop(float raw_speed,
                                  float filtered_speed,
                                  float deadband,
                                  int active_direction)
{
    const int filtered_direction =
        motor_dp_speed_sign(filtered_speed, deadband * DAMPING_STOP_RATIO);
    const int strong_raw_direction =
        motor_dp_speed_sign(raw_speed, deadband * DAMPING_RAW_OPPOSITE_RATIO);

    return filtered_direction == 0 ||
           (strong_raw_direction != 0 && strong_raw_direction != active_direction);
}

int32_t motor_dp_damping_torque_for_speed(int32_t damping_limit_ma,
                                          float damping_kp_ma_per_rpm,
                                          float damping_saturation_rpm,
                                          float damping_knee_rpm,
                                          float filtered_speed)
{
    if (damping_limit_ma <= 0 || damping_kp_ma_per_rpm <= 0.0f) {
        return 0;
    }

    const float speed = motor_dp_abs_float(filtered_speed);
    const float knee_torque =
        motor_dp_clamp_float(damping_kp_ma_per_rpm * damping_knee_rpm,
                             0.0f,
                             (float)damping_limit_ma);

    if (speed <= damping_knee_rpm || damping_saturation_rpm <= damping_knee_rpm) {
        return motor_dp_clamp_i32((int32_t)(damping_kp_ma_per_rpm * speed),
                                  0,
                                  damping_limit_ma);
    }
    if (speed >= damping_saturation_rpm) {
        return damping_limit_ma;
    }

    const float blend =
        motor_dp_smoothstep((speed - damping_knee_rpm) /
                            (damping_saturation_rpm - damping_knee_rpm));
    const float torque = knee_torque + ((float)damping_limit_ma - knee_torque) * blend;
    return motor_dp_clamp_i32((int32_t)torque, 0, damping_limit_ma);
}
