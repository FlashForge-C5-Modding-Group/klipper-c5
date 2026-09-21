// Startup and clock support for the N32G430F8S7
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stddef.h> // NULL
#ifdef N32G430_REGISTER_MODEL
#include "n32g430_register_model.h"
#else
#include "autoconf.h" // CONFIG_CLOCK_FREQ
#include "board/armcm_boot.h" // VectorTable
#include "internal.h" // struct cline
#include "sched.h" // sched_main
#endif

// Clock setup runs from the fixed 8MHz HSI.  Two million polls give at
// least 250ms even if a complete poll could execute in a single cycle.
#define N32G430_HSI_CLOCK_FREQ 8000000u
#define N32G430_CLOCK_TIMEOUT_MS 250u
#define N32G430_CLOCK_TIMEOUT \
    ((N32G430_HSI_CLOCK_FREQ / 1000u) * N32G430_CLOCK_TIMEOUT_MS)
#ifndef N32G430_WAIT_POLL
#define N32G430_WAIT_POLL(reg, mask, expected) do { } while (0)
#endif
#ifndef N32G430_RESET_REQUESTED
#define N32G430_RESET_REQUESTED() do { } while (0)
#endif

// Return the enable and reset controls for the peripherals supported here.
struct cline
lookup_clock_line(uint32_t periph_base)
{
    switch (periph_base) {
    case DMA1_BASE:
        return (struct cline){ .en=&RCC->AHBENR, .rst=NULL,
                              .bit=RCC_AHBENR_DMA1EN };
    case GPIOA_BASE:
        return (struct cline){ .en=&RCC->AHBENR, .rst=&RCC->AHBRSTR,
                              .bit=RCC_AHBENR_GPIOAEN };
    case GPIOB_BASE:
        return (struct cline){ .en=&RCC->AHBENR, .rst=&RCC->AHBRSTR,
                              .bit=RCC_AHBENR_GPIOBEN };
    case GPIOC_BASE:
        return (struct cline){ .en=&RCC->AHBENR, .rst=&RCC->AHBRSTR,
                              .bit=RCC_AHBENR_GPIOCEN };
    case GPIOD_BASE:
        return (struct cline){ .en=&RCC->AHBENR, .rst=&RCC->AHBRSTR,
                              .bit=RCC_AHBENR_GPIODEN };
    case TIM1_BASE:
        return (struct cline){ .en=&RCC->APB2ENR, .rst=&RCC->APB2RSTR,
                              .bit=RCC_APB2ENR_TIM1EN };
    case TIM8_BASE:
        return (struct cline){ .en=&RCC->APB2ENR, .rst=&RCC->APB2RSTR,
                              .bit=RCC_APB2ENR_TIM8EN };
    case USART1_BASE:
        return (struct cline){ .en=&RCC->APB2ENR, .rst=&RCC->APB2RSTR,
                              .bit=RCC_APB2ENR_USART1EN };
    default:
        return (struct cline){ .en=&RCC->AHBENR, .rst=&RCC->AHBRSTR };
    }
}

// Return the operating clock supplied to a supported peripheral.
uint32_t
get_pclock_frequency(uint32_t periph_base)
{
    switch (periph_base) {
    case USART1_BASE:
        return CONFIG_CLOCK_FREQ / 2;
    case TIM1_BASE:
    case TIM8_BASE:
    case DMA1_BASE:
    case GPIOA_BASE:
    case GPIOB_BASE:
    case GPIOC_BASE:
    case GPIOD_BASE:
        return CONFIG_CLOCK_FREQ;
    default:
        return 0;
    }
}

// Enable a GPIO bank without inferring gates for unimplemented banks.
void
gpio_clock_enable(GPIO_TypeDef *regs)
{
    uint32_t bit;
    if (regs == GPIOA)
        bit = RCC_AHBENR_GPIOAEN;
    else if (regs == GPIOB)
        bit = RCC_AHBENR_GPIOBEN;
    else if (regs == GPIOC)
        bit = RCC_AHBENR_GPIOCEN;
    else if (regs == GPIOD)
        bit = RCC_AHBENR_GPIODEN;
    else
        return;
    RCC->AHBENR |= bit;
    RCC->AHBENR;
}

void noinline __noreturn
n32g430_clock_fail(void)
{
    __disable_irq();
    __DSB();
    SCB->AIRCR = ((0x5fau << SCB_AIRCR_VECTKEY_Pos)
                  | (SCB->AIRCR & SCB_AIRCR_PRIGROUP_Msk)
                  | SCB_AIRCR_SYSRESETREQ_Msk);
    __DSB();
    N32G430_RESET_REQUESTED();
    for (;;)
        __NOP();
}

void noinline
n32g430_wait_mask_or_reset(volatile uint32_t *reg, uint32_t mask,
                           uint32_t expected)
{
    for (uint32_t timeout = N32G430_CLOCK_TIMEOUT; timeout; timeout--) {
        N32G430_WAIT_POLL(reg, mask, expected);
        if ((*reg & mask) == expected)
            return;
    }
    n32g430_clock_fail();
}

static void
clock_setup(void)
{
    // Revert to the HSI regardless of the clock state the boot stage
    // leaves behind; the PLL can not be disabled while it drives SYSCLK.
    RCC->CR |= RCC_CR_HSION;
    n32g430_wait_mask_or_reset(&RCC->CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY);
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_Msk) | RCC_CFGR_SW_HSI;
    n32g430_wait_mask_or_reset(&RCC->CFGR, RCC_CFGR_SWS_Msk,
                               RCC_CFGR_SWS_HSI);

    // Clock-tree fields may only change while the PLL is disabled.
    RCC->CR &= ~RCC_CR_PLLON;
    n32g430_wait_mask_or_reset(&RCC->CR, RCC_CR_PLLRDY, 0);

    // Establish the public 128MHz flash timing before raising SYSCLK.
    uint32_t acr = FLASH->ACR;
    acr &= ~(FLASH_ACR_LATENCY_Msk | FLASH_ACR_PRFTEN | FLASH_ACR_ICRST);
    FLASH->ACR = acr | FLASH_ACR_LATENCY_3 | FLASH_ACR_ICEN;

    // Use the crystal/resonator HSE path and wait until it is stable.
    RCC->CR = (RCC->CR & ~RCC_CR_HSEBYP) | RCC_CR_HSEON;
    n32g430_wait_mask_or_reset(&RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY);

    // HSE / 2 * 32 gives 128MHz HCLK; APB1 and APB2 are 32/64MHz.
    uint32_t cfgr = RCC->CFGR;
    cfgr &= ~(RCC_CFGR_SW_Msk | RCC_CFGR_HPRE_Msk
              | RCC_CFGR_PPRE1_Msk | RCC_CFGR_PPRE2_Msk
              | RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLXTPRE_HSE_DIV2
              | RCC_CFGR_PLLMUL_Msk);
    cfgr |= (RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2
             | RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLXTPRE_HSE_DIV2
             | RCC_CFGR_PLLMUL32);
    RCC->CFGR = cfgr;
    RCC->CFGR2 &= ~RCC_CFGR2_TIM1_8_SEL;

    RCC->CR |= RCC_CR_PLLON;
    n32g430_wait_mask_or_reset(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY);

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_Msk) | RCC_CFGR_SW_PLL;
    n32g430_wait_mask_or_reset(&RCC->CFGR, RCC_CFGR_SWS_Msk,
                               RCC_CFGR_SWS_PLL);
}

// Main entry point - called from armcm_boot.c:ResetHandler().
void
armcm_main(void)
{
    clock_setup();

    SCB->VTOR = (uint32_t)(uintptr_t)VectorTable;
    __DSB();
    __ISB();

    sched_main();
}
