// Reset command for the Creator 5 N32G430F8S7 levelboard
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "board/misc.h" // timer_read_time
#include "command.h" // DECL_COMMAND_FLAGS
#include "gpio.h" // gpio_out_setup
#include "internal.h" // NVIC_SystemReset

void
command_reset(uint32_t *args)
{
    (void)args;
    // PD0 doubles as BOOT0 and as the levelboard's pulled-up trigger input.
    // N32G430 re-latches BOOT0 on a software reset. An internal pull-down
    // alone is not enough to overcome the board's external pull-up on this
    // net, so actively drive PD0 low for the window around the reset - a
    // warm restart otherwise re-enters the bootloader instead of jumping to
    // the application.
    gpio_out_setup(GPIO('D', 0), 0);
    __DSB();
    uint32_t start = timer_read_time();
    while ((uint32_t)(timer_read_time() - start) < timer_from_us(1000))
        ;
    NVIC_SystemReset();
}
DECL_COMMAND_FLAGS(command_reset, HF_IN_SHUTDOWN, "reset");
