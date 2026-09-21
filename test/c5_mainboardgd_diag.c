// Creator 5 mainBoardGD diagnostic state regressions
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdint.h>
#include <stdio.h>

#include "c5_mainboardgd_diag.h"

static int failures;

static void
expect_u32(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected)
        return;
    fprintf(stderr, "%s: expected %u, got %u\n", name, expected, actual);
    failures++;
}

static void
test_independent_counters(void)
{
    struct c5_mainboardgd_diag snapshot;

    c5_mainboardgd_diag_clear();
    c5_mainboardgd_diag_isr(1, 0);
    c5_mainboardgd_diag_isr(1, 1);
    c5_mainboardgd_diag_idle(1);
    c5_mainboardgd_diag_isr(2, 0);
    c5_mainboardgd_diag_snapshot(&snapshot);

    expect_u32("Y ISR count", snapshot.motor[1].isr_count, 2);
    expect_u32("Y error count", snapshot.motor[1].error_count, 1);
    expect_u32("Y idle count", snapshot.motor[1].idle_count, 1);
    expect_u32("Z ISR count", snapshot.motor[2].isr_count, 1);
    expect_u32("X remains untouched", snapshot.motor[0].isr_count, 0);
}

static void
test_first_event_wins(void)
{
    struct c5_mainboardgd_diag snapshot;

    c5_mainboardgd_diag_clear();
    c5_mainboardgd_diag_event(2, C5_DIAG_EVENT_MCLIB_OUTPUT);
    c5_mainboardgd_diag_event(2, C5_DIAG_EVENT_COMPARE);
    c5_mainboardgd_diag_snapshot(&snapshot);

    expect_u32("first event", snapshot.motor[2].last_event,
               C5_DIAG_EVENT_MCLIB_OUTPUT);
}

static void
test_wrap_safe_timing(void)
{
    struct c5_mainboardgd_diag snapshot;

    c5_mainboardgd_diag_clear();
    c5_mainboardgd_diag_timing(0, 0xfffffff0u, 0x00000020u);
    c5_mainboardgd_diag_timing(0, 100u, 120u);
    c5_mainboardgd_diag_timing(0, 200u, 300u);
    c5_mainboardgd_diag_snapshot(&snapshot);

    expect_u32("wrap-safe elapsed maximum",
               snapshot.motor[0].max_isr_ticks, 100u);
}

int
main(void)
{
    test_independent_counters();
    test_first_event_wins();
    test_wrap_safe_timing();
    return failures ? 1 : 0;
}
