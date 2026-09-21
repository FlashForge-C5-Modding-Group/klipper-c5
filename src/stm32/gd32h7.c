// GD32H737 clock and core startup
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_FLASH_APPLICATION_ADDRESS
#include "board/irq.h" // irq_save
#include "command.h" // shutdown
#include "internal.h" // struct cline
#include "sched.h" // sched_main

static struct cline
clock_line(volatile uint32_t *en, volatile uint32_t *rst, uint32_t bit)
{
    return (struct cline){ .en = en, .rst = rst, .bit = bit };
}

// Map only peripherals whose GD32H737 clock lines are independently known.
struct cline
lookup_clock_line(uint32_t periph_base)
{
    switch (periph_base) {
    case GPIOA_BASE:
        return clock_line(&RCU_AHB4EN, &RCU_AHB4RST, RCU_AHB4EN_PAEN);
    case GPIOB_BASE:
        return clock_line(&RCU_AHB4EN, &RCU_AHB4RST, RCU_AHB4EN_PBEN);
    case GPIOC_BASE:
        return clock_line(&RCU_AHB4EN, &RCU_AHB4RST, RCU_AHB4EN_PCEN);
    case GPIOD_BASE:
        return clock_line(&RCU_AHB4EN, &RCU_AHB4RST, RCU_AHB4EN_PDEN);
    case GPIOE_BASE:
        return clock_line(&RCU_AHB4EN, &RCU_AHB4RST, RCU_AHB4EN_PEEN);
    case TIMER1:
        return clock_line(&RCU_APB1EN, &RCU_APB1RST, RCU_APB1EN_TIMER1EN);
    case TIMER2:
        return clock_line(&RCU_APB1EN, &RCU_APB1RST, RCU_APB1EN_TIMER2EN);
    case TIMER3:
        return clock_line(&RCU_APB1EN, &RCU_APB1RST, RCU_APB1EN_TIMER3EN);
    case TIMER4:
        return clock_line(&RCU_APB1EN, &RCU_APB1RST, RCU_APB1EN_TIMER4EN);
    case TIMER22:
        return clock_line(&RCU_APB1EN, &RCU_APB1RST, RCU_APB1EN_TIMER22EN);
    case TIMER7:
        return clock_line(&RCU_APB2EN, &RCU_APB2RST, RCU_APB2EN_TIMER7EN);
    case USART0_BASE:
        return clock_line(&RCU_APB2EN, &RCU_APB2RST, RCU_APB2EN_USART0EN);
    case ADC0:
        return clock_line(&RCU_APB2EN, &RCU_APB2RST, RCU_APB2EN_ADC0EN);
    case ADC1:
        return clock_line(&RCU_APB2EN, &RCU_APB2RST, RCU_APB2EN_ADC1EN);
    case ADC2:
        return clock_line(&RCU_APB2EN, &RCU_APB2RST, RCU_APB2EN_ADC2EN);
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
    case TIMER1: case TIMER2: case TIMER3:
    case TIMER4: case TIMER7: case TIMER22:
    case USART0_BASE: case ADC0: case ADC1: case ADC2:
        return 300000000;
    default:
        shutdown("Unknown peripheral clock");
        return 0;
    }
}

void
gpio_clock_enable(GPIO_TypeDef *regs)
{
    struct cline cl = lookup_clock_line((uint32_t)regs);
    if (cl.en != &RCU_AHB4EN)
        shutdown("Not a valid GPIO port");
    *cl.en |= cl.bit;
    *cl.en;
}

static void
clock_setup(void)
{
    // Move to IRC64M before changing either the PLL or TCM wait-state setup.
    RCU_CTL |= RCU_CTL_IRC64MEN;
    while (!(RCU_CTL & RCU_CTL_IRC64MSTB))
        ;
    RCU_CFG0 &= ~RCU_CFG0_SCS;
    while (RCU_CFG0 & RCU_CFG0_SCSS)
        ;

    RCU_APB4EN |= RCU_APB4EN_SYSCFGEN;
    SYSCFG_SRAMCFG1 &= ~SYSCFG_SRAMCFG1_TCM_WAITSTATE;

    RCU_CTL &= ~(RCU_CTL_PLL0EN | RCU_CTL_PLL1EN | RCU_CTL_PLL2EN);
    while (RCU_CTL & (RCU_CTL_PLL0STB | RCU_CTL_PLL1STB | RCU_CTL_PLL2STB))
        ;
    RCU_PLL0 = RCU_PLL0_RESET_VALUE;
    RCU_PLL1 = RCU_PLL1_RESET_VALUE;
    RCU_PLL2 = RCU_PLL2_RESET_VALUE;
    RCU_PLLADDCTL = 0;
    RCU_INT = RCU_INT_RESET_VALUE;

    RCU_CTL |= RCU_CTL_HXTALEN;
    while (!(RCU_CTL & RCU_CTL_HXTALSTB))
        ;

    // 25MHz / 5 * 120 / 1 = 600MHz. AHB/AXI run at 300MHz.
    RCU_CFG0 = RCU_AHB_CKSYS_DIV2 | RCU_APB1_CKAHB_DIV2
               | RCU_APB2_CKAHB_DIV1 | RCU_APB3_CKAHB_DIV2
               | RCU_APB4_CKAHB_DIV2;
    RCU_PLLALL = RCU_PLLSRC_HXTAL | RCU_PLL0RNG_4M_8M;
    RCU_PLL0 = 5U | ((120U - 1U) << RCU_PLLNOFFSET)
               | ((1U - 1U) << RCU_PLLPOFFSET)
               | ((2U - 1U) << RCU_PLLROFFSET);
    RCU_PLLADDCTL = ((2U - 1U) & RCU_PLLADDCTL_PLL0Q)
                    | RCU_PLL0P | RCU_PLL0Q | RCU_PLL0R;
    RCU_PLL0FRA = 0;
    RCU_PLL1FRA = 0;
    RCU_PLL2FRA = 0;

    RCU_CTL |= RCU_CTL_PLL0EN;
    while (!(RCU_CTL & RCU_CTL_PLL0STB))
        ;
    SYSCFG_SRAMCFG1 |= SYSCFG_SRAMCFG1_TCM_WAITSTATE;
    RCU_CFG0 = (RCU_CFG0 & ~RCU_CFG0_SCS) | RCU_CKSYSSRC_PLL0P;
    while ((RCU_CFG0 & RCU_CFG0_SCSS) != RCU_SCSS_PLL0P)
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
    SCB->VTOR = CONFIG_FLASH_APPLICATION_ADDRESS;
    __DSB();
    __ISB();
    sched_main();
}
