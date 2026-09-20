// Creator 5 mainBoardGD motor peripherals
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdint.h>
#include "board/irq.h" // irq_save
#include "command.h" // shutdown
#include "c5_mainboardgd.h" // c5_mainboardgd_control
#include "generic/armcm_boot.h" // armcm_enable_irq
#include "generic/armcm_timer.h" // udelay
#include "internal.h" // enable_pclock
#include "sched.h" // DECL_INIT

#define REG32(address) (*(volatile uint32_t *)(address))

#define DMA0_BASE       0x40020000UL
#define DMAMUX_BASE     0x40020800UL
#define TRIGSEL_BASE    0x40018400UL
#define ADC_COMMON      0x40012704UL

#define TIMER_CTL0      0x00
#define TIMER_CTL1      0x04
#define TIMER_SMCFG     0x08
#define TIMER_SWEVG     0x14
#define TIMER_CHCTL0    0x18
#define TIMER_CHCTL1    0x1c
#define TIMER_CHCTL2    0x20
#define TIMER_CNT       0x24
#define TIMER_PSC       0x28
#define TIMER_CAR       0x2c
#define TIMER_CREP      0x30
#define TIMER_CH0CV     0x34
#define TIMER_CCHP      0x44
#define TIMER_DCH0CV    0x64
#define TIMER_DCHCTL    0x74

#define ADC_STAT        0x00
#define ADC_CTL0        0x04
#define ADC_CTL1        0x08
#define ADC_RSQ0        0x24
#define ADC_RSQ1        0x40
#define ADC_RSQ2        0x44
#define ADC_ISQ         0x48
#define ADC_ISQ0        0x4c
#define ADC_IDATA0      0x54

#define ADC_STAT_ICEOC  (1U << 2)
#define ADC_CTL1_RSTCLB (1U << 3)
#define ADC_CTL1_CLB    (1U << 2)

#define MOTOR_IRQ_PRIORITY 2
#define DMA0_Channel0_IRQn 11
#define DMA0_Channel1_IRQn 12
#define ADC_IRQn           18

static const uint32_t pwm_bases[] = {
    TIMER3_BASE, TIMER7_BASE, TIMER1_BASE,
};
static const uint8_t compare_offsets[] = {
    0x34, 0x64, 0x38, 0x68, 0x3c, 0x6c, 0x40, 0x70,
};

static struct c5_mclib_acq acquisitions[3];
static volatile uint32_t dma_samples_y[2] __attribute__((aligned(4)));
static volatile uint32_t dma_samples_z[2] __attribute__((aligned(4)));

static inline volatile uint32_t *
reg_at(uint32_t base, uint32_t offset)
{
    return (volatile uint32_t *)(base + offset);
}

static uint32_t
pwm_base(uint8_t axis)
{
    if (axis >= 3)
        shutdown("Invalid mclib configuration");
    return pwm_bases[axis];
}

uint32_t
c5_mainboardgd_motor_time(void)
{
    return REG32(TIMER22_BASE + TIMER_CNT);
}

void
c5_mainboardgd_pwm_enable(uint8_t axis, uint8_t enable)
{
    volatile uint32_t *chctl2 = reg_at(pwm_base(axis), TIMER_CHCTL2);
    const uint32_t mask = 0x1111U;
    if (enable)
        *chctl2 |= mask;
    else
        *chctl2 &= ~mask;
}

static void
write_control_output(uint8_t axis, const struct c5_mclib_output *out)
{
    uint32_t base = pwm_base(axis);
    uint_fast8_t i;
    for (i = 0; i < 8; i++)
        REG32(base + compare_offsets[i]) = out->compare[i];
    c5_mclib_acq_polarity(&acquisitions[axis], out->signs);
}

static void
process_sample(uint8_t axis, int16_t raw0, int16_t raw1)
{
    float ia, ib;
    if (!c5_mclib_acquire(&acquisitions[axis], raw0, raw1, &ia, &ib))
        return;

    struct c5_mclib_output out;
    uint32_t now = c5_mainboardgd_motor_time();
    if (c5_mainboardgd_control(axis, now, ia, ib, &out))
        write_control_output(axis, &out);
}

void
DMA0_Channel0_IRQHandler(void)
{
    uint32_t status = REG32(DMA0_BASE + 0x00);
    if (!(status & 0x20U))
        return;
    REG32(DMA0_BASE + 0x08) = 0x3dU;
    int16_t raw0 = (int16_t)(uint16_t)dma_samples_y[0];
    int16_t raw1 = (int16_t)(uint16_t)dma_samples_y[1];
    process_sample(1, raw0, raw1);
}
DECL_ARMCM_IRQ(DMA0_Channel0_IRQHandler, DMA0_Channel0_IRQn);

void
DMA0_Channel1_IRQHandler(void)
{
    uint32_t status = REG32(DMA0_BASE + 0x00);
    if (!(status & 0x800U))
        return;
    REG32(DMA0_BASE + 0x08) = 0xf40U;
    int16_t raw0 = (int16_t)(uint16_t)dma_samples_z[0];
    int16_t raw1 = (int16_t)(uint16_t)dma_samples_z[1];
    process_sample(2, raw0, raw1);
}
DECL_ARMCM_IRQ(DMA0_Channel1_IRQHandler, DMA0_Channel1_IRQn);

void
ADC_IRQHandler(void)
{
    uint32_t stat0 = REG32(ADC0_BASE + ADC_STAT);
    if (stat0 & ADC_STAT_ICEOC) {
        int16_t raw0 = (int16_t)(uint16_t)REG32(ADC0_BASE + ADC_IDATA0);
        int16_t raw1 = (int16_t)(uint16_t)REG32(ADC0_BASE + ADC_IDATA0 + 4);
        REG32(ADC0_BASE + ADC_STAT) = ~ADC_STAT_ICEOC;
        process_sample(0, raw0, raw1);
    }
    if (REG32(ADC1_BASE + ADC_STAT) & ADC_STAT_ICEOC)
        REG32(ADC1_BASE + ADC_STAT) = ~ADC_STAT_ICEOC;
}
DECL_ARMCM_IRQ(ADC_IRQHandler, ADC_IRQn);

static void
setup_pwm_timer(uint32_t base)
{
    enable_pclock(base);
    REG32(base + TIMER_CTL0) &= ~0x370U;
    REG32(base + TIMER_PSC) = 0;
    REG32(base + TIMER_CAR) = 14999;
    REG32(base + TIMER_SWEVG) = 1U;

    REG32(base + TIMER_CHCTL0) =
        (REG32(base + TIMER_CHCTL0) & 0x0efe8484U) | 0x30006868U;
    REG32(base + TIMER_CHCTL1) =
        (REG32(base + TIMER_CHCTL1) & 0x0efe8484U) | 0x30006868U;
    REG32(base + TIMER_DCHCTL) |= 0xf0000000U;

    if (base == TIMER7_BASE) {
        REG32(base + TIMER_CHCTL2) &= ~0xffffU;
        REG32(base + TIMER_CTL1) &= ~0xff00U;
        REG32(base + TIMER_CREP) = 0;
        REG32(base + 0xfc) &= ~(1U << 2);
        REG32(base + TIMER_CCHP) |= 0x8000U;
    } else {
        REG32(base + TIMER_CHCTL2) &= ~0x3333U;
    }

    REG32(base + TIMER_CH0CV) = 0;
    REG32(base + TIMER_DCH0CV) = 0;
    REG32(base + TIMER_CH0CV + 4) = 0;
    REG32(base + TIMER_DCH0CV + 4) = 0;
    REG32(base + TIMER_CH0CV + 8) = 0;
    REG32(base + TIMER_DCH0CV + 8) = 0;
    REG32(base + TIMER_CH0CV + 12) = 0;
    REG32(base + TIMER_DCH0CV + 12) = 0;
}

static void
setup_trigger_timer(uint32_t base, const uint16_t compare[4])
{
    enable_pclock(base);
    REG32(base + TIMER_CTL0) &= ~0x370U;
    REG32(base + TIMER_PSC) = 0;
    REG32(base + TIMER_CAR) = 14999;
    REG32(base + TIMER_CHCTL0) =
        (REG32(base + TIMER_CHCTL0) & 0x3efe8484U) | 0x7878U;
    REG32(base + TIMER_CHCTL1) =
        (REG32(base + TIMER_CHCTL1) & 0x3efe8484U) | 0x7878U;
    REG32(base + TIMER_CHCTL2) =
        (REG32(base + TIMER_CHCTL2) & ~0x3333U) | 0x1111U;
    REG32(base + TIMER_CH0CV) = compare[0];
    REG32(base + TIMER_CH0CV + 4) = compare[1];
    REG32(base + TIMER_CH0CV + 8) = compare[2];
    REG32(base + TIMER_CH0CV + 12) = compare[3];
    if (base == TIMER2_BASE) {
        REG32(base + TIMER_SMCFG) |= 0x80U;
        REG32(base + TIMER_CTL1) =
            (REG32(base + TIMER_CTL1) & ~0x70U) | 0x10U;
    }
    REG32(base + TIMER_SWEVG) = 1U;
}

static void
setup_timer_routing(void)
{
    static const struct {
        uint32_t address, value;
    } routes[] = {
        { 0x58000500UL, 0x4c000000U },
        { 0x5800050cUL, 0x4c000000U },
        { 0x58000524UL, 0x4c000000U },
        { 0x5800053cUL, 0x4c000000U },
        { 0x58000530UL, 0x0c000000U },
    };
    static const struct {
        uint8_t selector, value;
    } trigsel[] = {
        { 0xa0, 0x2c }, { 0x10, 0x3a },
        { 0x98, 0x2d }, { 0x11, 0x3b },
        { 0x90, 0x2e }, { 0x14, 0x38 },
        { 0x8c, 0x2f }, { 0x15, 0x39 },
    };

    uint_fast8_t i;
    for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        REG32(routes[i].address + 0) = 0;
        REG32(routes[i].address + 4) = 0;
        REG32(routes[i].address + 8) = 0;
        REG32(routes[i].address) = routes[i].value;
    }
    for (i = 0; i < sizeof(trigsel) / sizeof(trigsel[0]); i++) {
        uint32_t address = TRIGSEL_BASE + (trigsel[i].selector & 0xfcU);
        uint32_t value = REG32(address);
        if (value & 0x80000000U)
            continue;
        uint32_t shift = (trigsel[i].selector & 3U) * 8U;
        value = (value & ~(0xffU << shift))
                | ((uint32_t)trigsel[i].value << shift);
        REG32(address) = value;
    }
}

static void
setup_motor_gpio(void)
{
    static const struct {
        uint8_t port, pin, function;
    } pwm_pins[] = {
        { 'B', 6, 2 }, { 'B', 7, 2 }, { 'B', 8, 2 }, { 'B', 9, 2 },
        { 'C', 6, 3 }, { 'C', 7, 3 }, { 'C', 8, 3 }, { 'C', 9, 3 },
        { 'A', 0, 1 }, { 'A', 1, 1 }, { 'A', 2, 1 }, { 'A', 3, 1 },
    };
    static const struct {
        uint8_t port, pin;
    } current_pins[] = {
        { 'B', 1 }, { 'B', 0 }, { 'C', 5 },
        { 'C', 4 }, { 'A', 7 }, { 'A', 6 },
    };

    uint_fast8_t i;
    for (i = 0; i < sizeof(pwm_pins) / sizeof(pwm_pins[0]); i++)
        gpio_peripheral(GPIO(pwm_pins[i].port, pwm_pins[i].pin),
                        GPIO_FUNCTION(pwm_pins[i].function), 0);
    for (i = 0; i < sizeof(current_pins) / sizeof(current_pins[0]); i++)
        gpio_peripheral(GPIO(current_pins[i].port, current_pins[i].pin),
                        GPIO_ANALOG, 0);
}

static void
setup_adc(uint32_t base, uint32_t regular0, uint32_t regular1,
          uint32_t inserted, uint32_t inserted0)
{
    REG32(base + ADC_CTL0) =
        (REG32(base + ADC_CTL0) & ~0x03000000U) | 0x180U;
    REG32(base + ADC_CTL1) = 0x10100301U;
    REG32(base + ADC_RSQ0) =
        (REG32(base + ADC_RSQ0) & ~0x00f00000U) | 0x00100000U;
    REG32(base + ADC_RSQ2) =
        (REG32(base + ADC_RSQ2) & ~0x7fffU) | regular0;
    REG32(base + ADC_RSQ1) =
        (REG32(base + ADC_RSQ1) & ~0x7fffU) | regular1;
    REG32(base + ADC_ISQ) =
        (REG32(base + ADC_ISQ) & ~0x00307fffU) | inserted;
    REG32(base + ADC_ISQ0) =
        (REG32(base + ADC_ISQ0) & ~0x7fff0000U) | inserted0;
    REG32(base + ADC_STAT) = ~ADC_STAT_ICEOC;

    udelay(100);
    REG32(base + ADC_CTL1) |= 1U << 27;
    REG32(base + ADC_CTL1) &= ~0x70U;
    REG32(base + ADC_CTL1) |= ADC_CTL1_RSTCLB;
    while (REG32(base + ADC_CTL1) & ADC_CTL1_RSTCLB)
        ;
    REG32(base + ADC_CTL1) |= ADC_CTL1_CLB;
    while (REG32(base + ADC_CTL1) & ADC_CTL1_CLB)
        ;
}

static void
setup_dma_channel(uint8_t channel, uint32_t source,
                  volatile uint32_t *destination, uint8_t request)
{
    uint32_t base = DMA0_BASE + 0x18U * channel;
    REG32(base + 0x10) = 0;
    REG32(base + 0x14) = 0;
    REG32(base + 0x18) = 0;
    REG32(base + 0x1c) = 0;
    REG32(base + 0x20) = 0;
    REG32(base + 0x24) = 0;
    REG32(base + 0x18) = source;
    REG32(base + 0x1c) = (uint32_t)destination;
    REG32(base + 0x14) = 2;

    uint32_t mux = DMAMUX_BASE + 4U * channel;
    REG32(mux) = (REG32(mux) & ~0xffU) | request;
    REG32(DMA0_BASE + 0x08) = channel ? 0xf40U : 0x3dU;
    REG32(base + 0x10) = 0x00035511U;
}

static void
setup_motor_time(void)
{
    enable_pclock(TIMER22_BASE);
    REG32(TIMER22_BASE + TIMER_CTL0) &= ~0x370U;
    REG32(TIMER22_BASE + TIMER_PSC) = 2;
    REG32(TIMER22_BASE + TIMER_CAR) = 0xffffffffU;
    REG32(TIMER22_BASE + TIMER_SWEVG) = 1U;
    REG32(TIMER22_BASE + TIMER_CTL0) |= 1U;
}

void
c5_mainboardgd_hardware_init(void)
{
    c5_mainboardgd_init_motors();
    uint_fast8_t axis;
    for (axis = 0; axis < 3; axis++)
        c5_mclib_acq_init(&acquisitions[axis]);

    REG32(RCU_BASE + 0x30) |= (1U << 21) | (1U << 23);
    REG32(RCU_BASE + 0x44) |= 1U << 31;
    REG32(RCU_BASE + 0x4c) |= 1U;

    setup_motor_gpio();
    setup_pwm_timer(TIMER3_BASE);
    setup_pwm_timer(TIMER7_BASE);
    setup_pwm_timer(TIMER1_BASE);

    static const uint16_t timer2_compare[4] = { 1000, 4750, 8500, 12250 };
    static const uint16_t timer4_compare[4] = { 1120, 4870, 8620, 12370 };
    setup_trigger_timer(TIMER2_BASE, timer2_compare);
    setup_trigger_timer(TIMER4_BASE, timer4_compare);
    setup_timer_routing();
    setup_motor_time();

    enable_pclock(ADC0_BASE);
    enable_pclock(ADC1_BASE);
    REG32(ADC_COMMON) =
        (REG32(ADC_COMMON) & ~0x00ff0000U) | 0x000a0000U;
    setup_adc(ADC0_BASE, 8, 4, 0x00100009U, 0x00050000U);
    setup_adc(ADC1_BASE, 7, 3, 0x00100013U, 0x00120000U);

    setup_dma_channel(0, ADC0_BASE + 0x64, dma_samples_y, 9);
    setup_dma_channel(1, ADC1_BASE + 0x64, dma_samples_z, 10);

    armcm_enable_irq(DMA0_Channel0_IRQHandler, DMA0_Channel0_IRQn,
                     MOTOR_IRQ_PRIORITY);
    armcm_enable_irq(DMA0_Channel1_IRQHandler, DMA0_Channel1_IRQn,
                     MOTOR_IRQ_PRIORITY);
    armcm_enable_irq(ADC_IRQHandler, ADC_IRQn, MOTOR_IRQ_PRIORITY);
    REG32(TIMER2_BASE + TIMER_CTL0) |= 1U;
}
DECL_INIT(c5_mainboardgd_hardware_init);

void
c5_mainboardgd_shutdown(void)
{
    irqstatus_t flag = irq_save();
    c5_mainboardgd_pwm_enable(0, 0);
    c5_mainboardgd_pwm_enable(1, 0);
    c5_mainboardgd_pwm_enable(2, 0);
    c5_mainboardgd_enable(0, 0);
    c5_mainboardgd_enable(1, 0);
    c5_mainboardgd_enable(2, 0);
    irq_restore(flag);
}
DECL_SHUTDOWN(c5_mainboardgd_shutdown);
