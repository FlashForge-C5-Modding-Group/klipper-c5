// Creator 5 mainBoardGD diagnostic state
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdint.h>
#include <string.h>
#include "c5_mainboardgd_diag.h"

static struct c5_mainboardgd_diag diagnostics;

void
c5_mainboardgd_diag_clear(void)
{
    memset(&diagnostics, 0, sizeof(diagnostics));
}

void
c5_mainboardgd_diag_isr(uint8_t axis, uint8_t error)
{
    diagnostics.motor[axis].isr_count++;
    diagnostics.motor[axis].error_count += !!error;
}

void
c5_mainboardgd_diag_idle(uint8_t axis)
{
    diagnostics.motor[axis].idle_count++;
}

void
c5_mainboardgd_diag_event(uint8_t axis, uint8_t event)
{
    if (!diagnostics.motor[axis].last_event)
        diagnostics.motor[axis].last_event = event;
}

void
c5_mainboardgd_diag_timing(uint8_t axis, uint32_t start, uint32_t end)
{
    uint32_t elapsed = end - start;
    if (elapsed > diagnostics.motor[axis].max_isr_ticks)
        diagnostics.motor[axis].max_isr_ticks = elapsed;
}

void
c5_mainboardgd_diag_snapshot(struct c5_mainboardgd_diag *snapshot)
{
    *snapshot = diagnostics;
}
