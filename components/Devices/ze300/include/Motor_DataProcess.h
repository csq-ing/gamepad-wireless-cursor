#pragma once

#include <stdbool.h>
#include <stdint.h>

int32_t motor_dp_clamp_i32(int32_t value, int32_t min_value, int32_t max_value);
uint32_t motor_dp_clamp_u32(uint32_t value, uint32_t min_value, uint32_t max_value);
float motor_dp_clamp_float(float value, float min_value, float max_value);
float motor_dp_abs_float(float value);
float motor_dp_smoothstep(float value);
int motor_dp_speed_sign(float speed, float deadband);
float motor_dp_wrap_angle_error_deg(float target_deg, float current_deg);
int32_t motor_dp_sanitize_torque_limit(int32_t value, int32_t hard_limit_ma);
int motor_dp_stable_motion_direction(float raw_speed, float filtered_speed, float deadband);
bool motor_dp_damping_should_stop(float raw_speed,
                                  float filtered_speed,
                                  float deadband,
                                  int active_direction);
int32_t motor_dp_damping_torque_for_speed(int32_t damping_limit_ma,
                                          float damping_kp_ma_per_rpm,
                                          float damping_saturation_rpm,
                                          float damping_knee_rpm,
                                          float filtered_speed);
