// GD32H737 clock and core startup
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_FLASH_APPLICATION_ADDRESS
#include "board/irq.h" // irq_save
#include "command.h" // shutdown
#include "internal.h" // struct cline
#include "sched.h" // sched_main

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define RCU_CTL             REG32(RCU_BASE + 0x00)
#define RCU_PLL0            REG32(RCU_BASE + 0x04)
#define RCU_CFG0            REG32(RCU_BASE + 0x08)
#define RCU_INT             REG32(RCU_BASE + 0x0c)
#define RCU_PLL1            REG32(RCU_BASE + 0x10)
#define RCU_PLL2            REG32(RCU_BASE + 0x14)
#define RCU_PLLADDCTL       REG32(RCU_BASE + 0x80)
#define RCU_PLL0FRA         REG32(RCU_BASE + 0x84)
#define RCU_PLL1FRA         REG32(RCU_BASE + 0x88)
#define RCU_PLL2FRA         REG32(RCU_BASE + 0x8c)
#define RCU_PLLALL          REG32(RCU_BASE + 0x98)
#define SYSCFG_SRAMCFG1     REG32(SYSCFG_BASE + 0x68)

#define RCU_CTL_HXTALEN     (1U << 16)
#define RCU_CTL_HXTALSTB    (1U << 17)
#define RCU_CTL_PLL0EN      (1U << 24)
#define RCU_CTL_PLL0STB     (1U << 25)
#define RCU_CTL_PLL1EN      (1U << 26)
#define RCU_CTL_PLL1STB     (1U << 27)
#define RCU_CTL_PLL2EN      (1U << 28)
#define RCU_CTL_PLL2STB     (1U << 29)
#define RCU_CTL_IRC64MEN    (1U << 30)
#define RCU_CTL_IRC64MSTB   (1U << 31)

static inline volatile uint32_t *
rcu_reg(uint32_t offset)
{
    return (volatile uint32_t *)(RCU_BASE + offset);
}

static struct cline
clock_line(uint32_t en_offset, uint32_t rst_offset, uint32_t bit)
{
    return (struct cline){
        .en = rcu_reg(en_offset), .rst = rcu_reg(rst_offset), .bit = bit
    };
}

// Map only peripherals whose GD32H737 clock lines are independently known.
struct cline
lookup_clock_line(uint32_t periph_base)
{
    switch (periph_base) {
    case GPIOA_BASE: return clock_line(0x3c, 0x1c, 1U << 0);
    case GPIOB_BASE: return clock_line(0x3c, 0x1c, 1U << 1);
    case GPIOC_BASE: return clock_line(0x3c, 0x1c, 1U << 2);
    case GPIOD_BASE: return clock_line(0x3c, 0x1c, 1U << 3);
    case GPIOE_BASE: return clock_line(0x3c, 0x1c, 1U << 4);
    case TIMER1_BASE: return clock_line(0x40, 0x20, 1U << 0);
    case TIMER2_BASE: return clock_line(0x40, 0x20, 1U << 1);
    case TIMER3_BASE: return clock_line(0x40, 0x20, 1U << 2);
    case TIMER4_BASE: return clock_line(0x40, 0x20, 1U << 3);
    case TIMER22_BASE: return clock_line(0x40, 0x20, 1U << 6);
    case TIMER7_BASE: return clock_line(0x44, 0x24, 1U << 1);
    case USART0_BASE: return clock_line(0x44, 0x24, 1U << 4);
    case ADC0_BASE: return clock_line(0x44, 0x24, 1U << 8);
    case ADC1_BASE: return clock_line(0x44, 0x24, 1U << 9);
    case ADC2_BASE: return clock_line(0x44, 0x24, 1U << 10);
    default:
        shutdown("Unknown peripheral clock");
        return (struct cline){};
    }
}

uint32_t
get_pclock_frequency(uint32_t periph_base)
{
    switch (periph_base) {
    case GPIOA_BASE: case GPIOB_BASE: case GPIOC_BASE:
    case GPIOD_BASE: case GPIOE_BASE:
    case TIMER1_BASE: case TIMER2_BASE: case TIMER3_BASE:
    case TIMER4_BASE: case TIMER7_BASE: case TIMER22_BASE:
    case USART0_BASE: case ADC0_BASE: case ADC1_BASE: case ADC2_BASE:
        return 300000000;
    default:
        shutdown("Unknown peripheral clock");
        return 0;
    }
}

void
gpio_clock_enable(GPIO_TypeDef *regs)
{
    uint32_t port = ((uint32_t)regs - GPIOA_BASE) / 0x400;
    if (port >= 5 || (uint32_t)regs != GPIOA_BASE + port * 0x400)
        shutdown("Not a valid GPIO port");
    *rcu_reg(0x3c) |= 1U << port;
    *rcu_reg(0x3c);
}

static void
clock_setup(void)
{
    // Move to IRC64M before changing either the PLL or TCM wait-state setup.
    RCU_CTL |= RCU_CTL_IRC64MEN;
    while (!(RCU_CTL & RCU_CTL_IRC64MSTB))
        ;
    RCU_CFG0 &= ~3U;
    while (RCU_CFG0 & 0x0c)
        ;

    *rcu_reg(0x4c) |= 1U << 0;
    SYSCFG_SRAMCFG1 &= ~1U;

    RCU_CTL &= ~(RCU_CTL_PLL0EN | RCU_CTL_PLL1EN | RCU_CTL_PLL2EN);
    while (RCU_CTL & (RCU_CTL_PLL0STB | RCU_CTL_PLL1STB | RCU_CTL_PLL2STB))
        ;
    RCU_PLL0 = 0x01002020;
    RCU_PLL1 = 0x01012020;
    RCU_PLL2 = 0x01012020;
    RCU_PLLADDCTL = 0;
    RCU_PLL0FRA = 0;
    RCU_PLL1FRA = 0;
    RCU_PLL2FRA = 0;
    RCU_INT = 0x14ff0000;

    RCU_CTL |= RCU_CTL_HXTALEN;
    while (!(RCU_CTL & RCU_CTL_HXTALSTB))
        ;

    // 25MHz / 5 * 120 / 1 = 600MHz. AHB/AXI run at 300MHz.
    RCU_CFG0 = 0x24001080;
    RCU_PLLALL = 0x00020002;
    RCU_PLL0 = 0x01001dc5;
    RCU_PLLADDCTL = 0x03800001;
    RCU_PLL0FRA = 0;

    RCU_CTL |= RCU_CTL_PLL0EN;
    while (!(RCU_CTL & RCU_CTL_PLL0STB))
        ;
    SYSCFG_SRAMCFG1 |= 1U;
    RCU_CFG0 = (RCU_CFG0 & ~3U) | 3U;
    while ((RCU_CFG0 & 0x0c) != 0x0c)
        ;
}

void
armcm_main(void)
{
    irqstatus_t flag = irq_save();
    SCB->CPACR |= 0x0fU << 20;
    __DSB();
    __ISB();
    FPU->FPCCR = (FPU->FPCCR | FPU_FPCCR_ASPEN_Msk)
                 & ~FPU_FPCCR_LSPEN_Msk;
    __DSB();
    __ISB();
    irq_restore(flag);

    clock_setup();
    SCB_EnableICache();
    SCB->VTOR = 0x08000000;
    __DSB();
    __ISB();
    sched_main();
}
