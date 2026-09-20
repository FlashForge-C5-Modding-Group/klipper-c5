// Free watchdog support for GD32H737
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "internal.h" // RCU_BASE
#include "sched.h" // DECL_TASK

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define RCU_RSTSCK       REG32(RCU_BASE + 0x74)
#define FWDGT_CTL        REG32(FWDGT_BASE + 0x00)
#define FWDGT_PSC        REG32(FWDGT_BASE + 0x04)
#define FWDGT_RLD        REG32(FWDGT_BASE + 0x08)
#define FWDGT_STAT       REG32(FWDGT_BASE + 0x0c)
#define FWDGT_WND        REG32(FWDGT_BASE + 0x10)

void
watchdog_reset(void)
{
    FWDGT_CTL = 0xaaaa;
}
DECL_TASK(watchdog_reset);

void
watchdog_init(void)
{
    RCU_RSTSCK |= 1U << 0;
    while (!(RCU_RSTSCK & (1U << 1)))
        ;
    FWDGT_CTL = 0x5555;
    FWDGT_PSC = 0;
    FWDGT_RLD = 0x0fff;
    FWDGT_WND = 0x0fff;
    while (FWDGT_STAT)
        ;
    FWDGT_CTL = 0xaaaa;
    FWDGT_CTL = 0xcccc;
}
DECL_INIT(watchdog_init);
