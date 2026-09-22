#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N32G430_REGISTER_MODEL 1
#include "n32g430_register_model.h"
#include "../src/stm32/n32g430.c"
#include "../src/stm32/clockline.c"
#include "../src/stm32/c5_levelboard.c"
_Static_assert(N32G430_CLOCK_TIMEOUT > N32G430_HSI_CLOCK_FREQ / 10u,
               "clock polling must exceed 100ms at the HSI frequency");

RCC_TypeDef model_rcc;
FLASH_TypeDef model_flash;
SCB_Type model_scb;
GPIO_TypeDef model_gpio_ports[4];
TIM_TypeDef model_tim1, model_tim8;
DMA_TypeDef model_dma1;
DMA_Channel_TypeDef model_dma1_channel1;
uint32_t VectorTable[1];

static jmp_buf reset_jump;
static int stalled_wait;
static int wait_index;
static volatile uint32_t *last_wait_reg;
static uint32_t last_wait_mask, last_wait_expected;
static int reset_requested;
static int scheduler_reached;
static uint16_t captured_delta;
static unsigned capture_count;
static void (*installed_irq)(void);
static IRQn_Type installed_irqn;
static uint32_t installed_priority;
static unsigned dma_write_count;
static int unsafe_dma_write;
static volatile uint32_t *dma_write_regs[12];
static uint32_t dma_write_values[12];

#define SEEDED_PRIGROUP (5u << 8)

static int
fail(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected)
        return 0;
    fprintf(stderr, "%s: expected 0x%08x, got 0x%08x\n",
            name, expected, actual);
    return 1;
}

void
n32g430_model_poll(volatile uint32_t *reg, uint32_t mask,
                    uint32_t expected)
{
    if (reg != last_wait_reg || mask != last_wait_mask
        || expected != last_wait_expected) {
        wait_index++;
        last_wait_reg = reg;
        last_wait_mask = mask;
        last_wait_expected = expected;
    }
    if (wait_index == stalled_wait)
        return;
    *reg = (*reg & ~mask) | expected;
}

void
n32g430_model_reset_requested(void)
{
    uint32_t aircr = model_scb.AIRCR;
    reset_requested = ((aircr & SCB_AIRCR_VECTKEY_Msk)
                       == (0x5fau << SCB_AIRCR_VECTKEY_Pos)
                       && (aircr & SCB_AIRCR_SYSRESETREQ_Msk)
                       && (aircr & SCB_AIRCR_PRIGROUP_Msk)
                          == SEEDED_PRIGROUP);
    longjmp(reset_jump, 1);
}

void
n32g430_model_dma_channel_write(volatile uint32_t *reg, uint32_t value)
{
    // While active, the only legal channel write is an exact EN clear.
    uint32_t control = model_dma1_channel1.CCR;
    if ((control & DMA_CCR_EN)
        && (reg != &model_dma1_channel1.CCR
            || value != (control & ~DMA_CCR_EN)))
        unsafe_dma_write = 1;
    if (dma_write_count < sizeof(dma_write_regs) / sizeof(dma_write_regs[0])) {
        dma_write_regs[dma_write_count] = reg;
        dma_write_values[dma_write_count] = value;
    }
    dma_write_count++;
    *reg = value;
}

void model_disable_irq(void) { }
void model_dsb(void) { }
void model_isb(void) { }
void model_nop(void) { }
irqstatus_t irq_save(void) { return 0; }
void irq_restore(irqstatus_t flag) { (void)flag; }
void gpio_peripheral(uint32_t gpio, uint32_t mode, int pullup)
{
    (void)gpio;
    (void)mode;
    (void)pullup;
}
void
armcm_enable_irq(void (*func)(void), IRQn_Type irq, uint32_t priority)
{
    installed_irq = func;
    installed_irqn = irq;
    installed_priority = priority;
}
void
c5_levelboard_capture(uint16_t delta)
{
    captured_delta = delta;
    capture_count++;
}
void sched_main(void) { scheduler_reached = 1; }

static void
reset_model(int stall)
{
    memset(&model_rcc, 0, sizeof(model_rcc));
    model_rcc.CR = RCC_CR_PLLON | RCC_CR_PLLRDY;
    model_rcc.CFGR = RCC_CFGR_SW_PLL | RCC_CFGR_SWS_PLL;
    memset(&model_flash, 0, sizeof(model_flash));
    memset(&model_scb, 0, sizeof(model_scb));
    model_scb.AIRCR = SEEDED_PRIGROUP;
    memset(model_gpio_ports, 0, sizeof(model_gpio_ports));
    memset(&model_tim1, 0, sizeof(model_tim1));
    memset(&model_tim8, 0, sizeof(model_tim8));
    memset(&model_dma1, 0, sizeof(model_dma1));
    memset(&model_dma1_channel1, 0, sizeof(model_dma1_channel1));
    stalled_wait = stall;
    wait_index = 0;
    last_wait_reg = NULL;
    last_wait_mask = 0;
    last_wait_expected = 0;
    reset_requested = 0;
    scheduler_reached = 0;
    dma_write_count = 0;
    unsafe_dma_write = 0;
}

static uint32_t
decode_hclk(void)
{
    uint32_t cfgr = model_rcc.CFGR;
    uint32_t pll_input = 8000000u;
    if (cfgr & RCC_CFGR_PLLXTPRE_HSE_DIV2)
        pll_input /= 2u;
    uint32_t multiplier = ((cfgr & (1u << 27))
                           ? ((cfgr >> 18) & 0x0fu) + 17u
                           : ((cfgr >> 18) & 0x0fu) + 2u);
    uint32_t hclk = pll_input * multiplier;
    static const uint16_t divisors[8] = {2, 4, 8, 16, 64, 128, 256, 512};
    uint32_t hpre = (cfgr >> 4) & 0x0fu;
    if (hpre >= 8)
        hclk /= divisors[hpre - 8];
    return hclk;
}

static uint32_t
decode_pclock(uint32_t shift)
{
    uint32_t prescaler = (model_rcc.CFGR >> shift) & 0x07u;
    if (prescaler < 4)
        return decode_hclk();
    return decode_hclk() / (2u << (prescaler - 4u));
}

static int
run_normal_clock_path(void)
{
    int failures = 0;
    reset_model(0);
    if (setjmp(reset_jump)) {
        fprintf(stderr, "normal clock path unexpectedly reset\n");
        return 1;
    }
    armcm_main();
    failures += fail("normal wait count", wait_index, 6);
    failures += fail("normal scheduler reached", scheduler_reached, 1);
    failures += fail("normal reset absent", reset_requested, 0);
    failures += fail("HCLK", decode_hclk(), 128000000u);
    failures += fail("PCLK1", decode_pclock(8), 32000000u);
    failures += fail("PCLK2", decode_pclock(11), 64000000u);
    failures += fail("PLL selected",
                     model_rcc.CFGR & RCC_CFGR_SWS_Msk, RCC_CFGR_SWS_PLL);
    failures += fail("HSE PLL source",
                     model_rcc.CFGR & RCC_CFGR_PLLSRC_HSE,
                     RCC_CFGR_PLLSRC_HSE);
    failures += fail("HSE divided by two",
                     model_rcc.CFGR & RCC_CFGR_PLLXTPRE_HSE_DIV2,
                     RCC_CFGR_PLLXTPRE_HSE_DIV2);
    failures += fail("PLL multiplier",
                     model_rcc.CFGR & RCC_CFGR_PLLMUL_Msk,
                     RCC_CFGR_PLLMUL32);
    failures += fail("TIM1/TIM8 use 128MHz timer clock",
                     model_rcc.CFGR2 & RCC_CFGR2_TIM1_8_SEL, 0);
    failures += fail("flash latency and cache", model_flash.ACR,
                     FLASH_ACR_LATENCY_3 | FLASH_ACR_ICEN);
    failures += fail("USART1 reported PCLK2",
                     get_pclock_frequency(USART1_BASE), 64000000u);
    failures += fail("TIM1 reported timer clock",
                     get_pclock_frequency(TIM1_BASE), 128000000u);
    failures += fail("IWDG has no false pclock",
                     get_pclock_frequency(IWDG_BASE), 0);
    return failures;
}

static int
run_clock_timeout(int stall)
{
    int failures = 0;
    reset_model(stall);
    if (!setjmp(reset_jump)) {
        armcm_main();
        fprintf(stderr, "clock wait %d returned instead of resetting\n", stall);
        return 1;
    }
    failures += fail("timeout reset requested", reset_requested, 1);
    failures += fail("timeout scheduler blocked", scheduler_reached, 0);
    failures += fail("timeout stopped at requested wait", wait_index,
                     (uint32_t)stall);
    failures += fail("timeout SYSRESETREQ",
                     model_scb.AIRCR & SCB_AIRCR_SYSRESETREQ_Msk,
                     SCB_AIRCR_SYSRESETREQ_Msk);
    failures += fail("timeout reset key",
                     model_scb.AIRCR & SCB_AIRCR_VECTKEY_Msk,
                     0x5fau << SCB_AIRCR_VECTKEY_Pos);
    failures += fail("timeout preserves PRIGROUP",
                     model_scb.AIRCR & SCB_AIRCR_PRIGROUP_Msk,
                     SEEDED_PRIGROUP);
    return failures;
}

static int
run_warm_dma_path(void)
{
    int failures = 0;
    reset_model(0);
    const uint32_t reset_sentinel = 0xa55a5aa5u;
    model_rcc.AHBRSTR = reset_sentinel;
    model_dma1.ISR = 0x0fu;
    model_dma1.IFCR = 0xa5u;
    model_dma1_channel1.CCR = 0xffffu;
    model_dma1_channel1.CNDTR = 0x1234u;
    model_dma1_channel1.CPAR = 0x11111111u;
    model_dma1_channel1.CMAR = 0x22222222u;
    model_dma1_channel1.CHSEL = 0x3fu;
    installed_irq = NULL;
    installed_irqn = -1;
    installed_priority = UINT32_MAX;
    capture_count = 0;
    captured_delta = 0;
    previous_snapshot = 0;

    c5_levelboard_acquisition_init();
    volatile uint32_t *expected_regs[] = {
        &model_dma1_channel1.CCR, &model_dma1_channel1.CCR,
        &model_dma1_channel1.CPAR, &model_dma1_channel1.CMAR,
        &model_dma1_channel1.CNDTR, &model_dma1_channel1.CHSEL,
        &model_dma1_channel1.CCR, &model_dma1_channel1.CCR,
        &model_dma1_channel1.CCR,
    };
    uint32_t expected_values[] = {
        0xfffeu, 0, (uint32_t)(uintptr_t)&model_tim1.CNT,
        (uint32_t)(uintptr_t)&dma_snapshot, 1, 0x32u, 0x2520u, 0x2522u,
        0x2523u,
    };
    failures += fail("DMA write count", dma_write_count,
                     sizeof(expected_regs) / sizeof(expected_regs[0]));
    failures += fail("DMA writes only while disabled", unsafe_dma_write, 0);
    unsigned compared_writes = dma_write_count;
    if (compared_writes > sizeof(expected_regs) / sizeof(expected_regs[0]))
        compared_writes = sizeof(expected_regs) / sizeof(expected_regs[0]);
    for (unsigned i = 0; i < compared_writes; i++) {
        failures += fail("DMA write register order",
                         dma_write_regs[i] == expected_regs[i], 1);
        failures += fail("DMA write value order", dma_write_values[i],
                         expected_values[i]);
    }

    failures += fail("DMA clock enabled",
                     model_rcc.AHBENR & RCC_AHBENR_DMA1EN,
                     RCC_AHBENR_DMA1EN);
    failures += fail("reserved AHBRSTR untouched", model_rcc.AHBRSTR,
                     reset_sentinel);
    failures += fail("all channel 1 flags cleared", model_dma1.IFCR, 0x0fu);
    failures += fail("DMA CPAR", model_dma1_channel1.CPAR,
                     (uint32_t)(uintptr_t)&model_tim1.CNT);
    failures += fail("DMA CMAR", model_dma1_channel1.CMAR,
                     (uint32_t)(uintptr_t)&dma_snapshot);
    failures += fail("DMA count", model_dma1_channel1.CNDTR, 1);
    failures += fail("DMA request", model_dma1_channel1.CHSEL, 0x32u);
    failures += fail("DMA final control", model_dma1_channel1.CCR, 0x2523u);
    failures += fail("DMA IRQ number", (uint32_t)installed_irqn,
                     DMA1_Channel1_IRQn);
    failures += fail("DMA IRQ priority", installed_priority, 0);
    failures += fail("DMA IRQ handler installed",
                     installed_irq == DMA1_Channel1_IRQHandler, 1);

    model_dma1_channel1.CCR = 0x2523u;
    model_dma1.IFCR = 0;
    model_dma1.ISR = DMA_ISR_TCIF1;
    dma_snapshot = 5;
    installed_irq();
    failures += fail("IRQ clears TC only", model_dma1.IFCR,
                     DMA_IFCR_CTCIF1);
    failures += fail("IRQ first modulo delta", captured_delta, 5);
    failures += fail("IRQ leaves DMA enabled", model_dma1_channel1.CCR,
                     0x2523u);

    model_dma1.IFCR = 0;
    dma_snapshot = 0;
    installed_irq();
    failures += fail("IRQ wrapped modulo delta", captured_delta, 65531u);
    failures += fail("IRQ capture count", capture_count, 2);
    failures += fail("IRQ still clears TC only", model_dma1.IFCR,
                     DMA_IFCR_CTCIF1);
    return failures;
}

int
main(void)
{
    int failures = run_normal_clock_path();
    for (int stall = 1; stall <= 6; stall++)
        failures += run_clock_timeout(stall);
    failures += run_warm_dma_path();
    return failures ? 1 : 0;
}
