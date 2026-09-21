// Creator 5 mainBoardGD diagnostic state
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#ifndef __C5_MAINBOARDGD_DIAG_H
#define __C5_MAINBOARDGD_DIAG_H

#include <stdint.h>

#define C5_MAINBOARDGD_MOTOR_COUNT 3

enum c5_mainboardgd_diag_event {
    C5_DIAG_EVENT_NONE = 0,
    C5_DIAG_EVENT_MCLIB_OUTPUT = 1,
    C5_DIAG_EVENT_SIGNS = 2,
    C5_DIAG_EVENT_COMPARE = 3,
    C5_DIAG_EVENT_DMA = 4,
    C5_DIAG_EVENT_ADC_OVERRUN = 5,
};

struct c5_mainboardgd_motor_diag {
    uint32_t isr_count;
    uint32_t error_count;
    uint32_t idle_count;
    uint32_t max_isr_ticks;
    uint8_t last_event;
};

struct c5_mainboardgd_diag {
    struct c5_mainboardgd_motor_diag motor[C5_MAINBOARDGD_MOTOR_COUNT];
};

void c5_mainboardgd_diag_clear(void);
void c5_mainboardgd_diag_isr(uint8_t axis, uint8_t error);
void c5_mainboardgd_diag_idle(uint8_t axis);
void c5_mainboardgd_diag_event(uint8_t axis, uint8_t event);
void c5_mainboardgd_diag_timing(uint8_t axis, uint32_t start, uint32_t end);
void c5_mainboardgd_diag_snapshot(struct c5_mainboardgd_diag *snapshot);

#endif // c5_mainboardgd_diag.h
