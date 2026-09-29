// Reset command for the Creator 5 N32G430F8S7 levelboard
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "board/misc.h" // timer_read_time
#include "command.h" // DECL_COMMAND_FLAGS
#include "internal.h" // gpio_peripheral, NVIC_SystemReset

void
command_reset(uint32_t *args)
{
    (void)args;
    // PD0 doubles as BOOT0 and as the levelboard's pulled-up trigger input.
    // N32G430 re-latches BOOT0 on a software reset. Return PD0 to its reset
    // input pull-down state before asking for that reset, or a warm restart
    // may enter the ROM bootloader instead of the application.
    gpio_peripheral(GPIO('D', 0), GPIO_INPUT, -1);
    __DSB();
    uint32_t start = timer_read_time();
    while ((uint32_t)(timer_read_time() - start) < timer_from_us(1000))
        ;
    NVIC_SystemReset();
}
DECL_COMMAND_FLAGS(command_reset, HF_IN_SHUTDOWN, "reset");
