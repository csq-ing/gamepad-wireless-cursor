#pragma once

#include "gamepad_common.h"
#include "receiver_config_protocol.h"

void receiver_status_init(void);
void receiver_status_on_input(const gamepad_packet_t *pkt);
void receiver_status_get_snapshot(receiver_status_snapshot_t *snapshot);

