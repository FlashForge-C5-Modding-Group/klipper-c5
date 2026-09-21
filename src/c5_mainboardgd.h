// Creator 5 mainBoardGD application and hardware boundary
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#ifndef __C5_MAINBOARDGD_H
#define __C5_MAINBOARDGD_H

#include <stdint.h>
#include "c5_mclib.h"

void c5_mainboardgd_init_motors(void);
void c5_mainboardgd_direction(uint8_t axis, uint8_t value);
void c5_mainboardgd_step(uint8_t axis);
void c5_mainboardgd_enable(uint8_t axis, uint8_t enable);
uint8_t c5_mainboardgd_stalled(uint8_t axis);
uint8_t c5_mainboardgd_control(uint8_t axis, uint32_t now,
                              float ia, float ib,
                              struct c5_mclib_output *out);
uint32_t c5_mainboardgd_motor_time(void);
void c5_mainboardgd_pwm_enable(uint8_t axis, uint8_t enable);

#define C5_MAINBOARDGD_DIAG_PERSIST_VALID 0x01U
#define C5_MAINBOARDGD_DIAG_HARDFAULT     0x02U
#define C5_MAINBOARDGD_DIAG_SHUTDOWN      0x04U
#define C5_MAINBOARDGD_DIAG_CURRENT_STOP  0x08U

struct c5_mainboardgd_hw_diag {
    uint32_t reset_status;
    uint32_t fault_pc;
    uint32_t fault_lr;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t boot_count;
    uint32_t flags;
    uint32_t adc_stat0;
    uint32_t adc_stat1;
    uint32_t dma_intf;
    uint8_t previous_event[3];
};

void c5_mainboardgd_hw_diag_snapshot(struct c5_mainboardgd_hw_diag *snapshot);
void c5_mainboardgd_hw_diag_event(uint8_t axis, uint8_t event);
void c5_mainboardgd_hw_diag_shutdown(void);
#endif // c5_mainboardgd.h
