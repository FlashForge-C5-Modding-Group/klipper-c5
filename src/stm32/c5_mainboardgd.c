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

#define DMA0_BASE                       0x40020000UL
#define DMAMUX_BASE                     0x40020800UL
#define TRIGSEL_BASE                    0x40018400UL
#define ADC0_SYNCCTL                    0x40012704UL

#define SYSCFG_TIMER0CFG0_OFFSET        0x100U
#define SYSCFG_TIMER1CFG0_OFFSET        0x10cU
#define SYSCFG_TIMER3CFG0_OFFSET        0x124U
#define SYSCFG_TIMER4CFG0_OFFSET        0x130U
#define SYSCFG_TIMER7CFG0_OFFSET        0x13cU
#define SYSCFG_TSCFG5_EVENT_ITI14       0x4c000000U
#define SYSCFG_TSCFG5_EVENT_ITI2        0x0c000000U

#define TRIGSEL_ADC0_OFFSET             0x10U
#define TRIGSEL_ADC1_OFFSET             0x14U
#define TRIGSEL_TIMER0ITI14_OFFSET      0x8cU
#define TRIGSEL_TIMER1ITI14_OFFSET      0x90U
#define TRIGSEL_TIMER3ITI14_OFFSET      0x98U
#define TRIGSEL_TIMER7ITI14_OFFSET      0xa0U
#define TRIGSEL_TARGET_LK               (1U << 31)
#define TRIGSEL_INSEL_MASK(index)       (0xffU << (8U * (index)))
#define TRIGSEL_IN_TIMER2_CH0           0x2cU
#define TRIGSEL_IN_TIMER2_CH1           0x2dU
#define TRIGSEL_IN_TIMER2_CH2           0x2eU
#define TRIGSEL_IN_TIMER2_CH3           0x2fU
#define TRIGSEL_IN_TIMER4_CH0           0x38U
#define TRIGSEL_IN_TIMER4_CH1           0x39U
#define TRIGSEL_IN_TIMER4_CH2           0x3aU
#define TRIGSEL_IN_TIMER4_CH3           0x3bU

#define TIMER_CTL0_OFFSET               0x00U
#define TIMER_CTL1_OFFSET               0x04U
#define TIMER_SMCFG_OFFSET              0x08U
#define TIMER_SWEVG_OFFSET              0x14U
#define TIMER_CHCTL0_OFFSET             0x18U
#define TIMER_CHCTL1_OFFSET             0x1cU
#define TIMER_CHCTL2_OFFSET             0x20U
#define TIMER_CNT_OFFSET                0x24U
#define TIMER_PSC_OFFSET                0x28U
#define TIMER_CAR_OFFSET                0x2cU
#define TIMER_CREP0_OFFSET              0x30U
#define TIMER_CH0CV_OFFSET              0x34U
#define TIMER_CH1CV_OFFSET              0x38U
#define TIMER_CH2CV_OFFSET              0x3cU
#define TIMER_CH3CV_OFFSET              0x40U
#define TIMER_CCHP_OFFSET               0x44U
#define TIMER_CH0COMV_ADD_OFFSET        0x64U
#define TIMER_CH1COMV_ADD_OFFSET        0x68U
#define TIMER_CH2COMV_ADD_OFFSET        0x6cU
#define TIMER_CH3COMV_ADD_OFFSET        0x70U
#define TIMER_CTL2_OFFSET               0x74U
#define TIMER_CFG_OFFSET                0xfcU

#define TIMER_CTL0_COUNT_MODE_MASK      0x370U
#define TIMER_CHCTL2_CHANNEL_ENABLE_MASK 0x1111U
#define TIMER_CTL2_CH0CPWMEN            (1U << 28)
#define TIMER_CTL2_CH1CPWMEN            (1U << 29)
#define TIMER_CTL2_CH2CPWMEN            (1U << 30)
#define TIMER_CTL2_CH3CPWMEN            (1U << 31)
#define TIMER_CTL2_CHXCPWMEN_MASK       (TIMER_CTL2_CH0CPWMEN \
                                         | TIMER_CTL2_CH1CPWMEN \
                                         | TIMER_CTL2_CH2CPWMEN \
                                         | TIMER_CTL2_CH3CPWMEN)
#define TIMER_CFG_CREPSEL               (1U << 2)
#define TIMER_CCHP_POEN                 (1U << 15)
#define TIMER_SMCFG_MSM                 (1U << 7)
#define TIMER_CTL1_MMC0_MASK            (7U << 4)
#define TIMER_CTL1_MMC0(value)          ((uint32_t)(value) << 4)
#define TIMER_CAR_20KHZ                 14999U

#define ADC_STAT_OFFSET                 0x00U
#define ADC_CTL0_OFFSET                 0x04U
#define ADC_CTL1_OFFSET                 0x08U
#define ADC_RSQ0_OFFSET                 0x24U
#define ADC_RSQ7_OFFSET                 0x40U
#define ADC_RSQ8_OFFSET                 0x44U
#define ADC_ISQ0_OFFSET                 0x48U
#define ADC_ISQ1_OFFSET                 0x4cU
#define ADC_IDATA0_OFFSET               0x54U
#define ADC_RDATA_OFFSET                0x64U

#define ADC_STAT_EOIC                   (1U << 2)
#define ADC_CTL1_RSTCLB                 (1U << 3)
#define ADC_CTL1_CLB                    (1U << 2)
#define ADC_RSQ0_RL_MASK                (0x0fU << 20)
#define ADC_RSQ0_RL(count)              (((uint32_t)(count) - 1U) << 20)
#define ADC_RSQ8_RANK0_MASK             0x7fffU
#define ADC_RSQ7_RANK1_MASK             0x7fffU
#define ADC_RSQ_RANK(channel)           ((uint32_t)(channel) & 0x1fU)
#define ADC_ISQ0_IL_MASK                (3U << 20)
#define ADC_ISQ0_IL(count)              (((uint32_t)(count) - 1U) << 20)
#define ADC_ISQ0_RANK1_MASK             0x7fffU
#define ADC_ISQ0_RANK1(channel)         ((uint32_t)(channel) & 0x1fU)
#define ADC_ISQ1_RANK0_MASK             (0x7fffU << 16)
#define ADC_ISQ1_RANK0(channel)         (((uint32_t)(channel) & 0x1fU) << 16)

#define DMA_INTF0                       0x00U
#define DMA_INTC0                       0x08U
#define DMA_CHCTL(channel)              (0x10U + 0x18U * (channel))
#define DMA_CHCNT(channel)              (0x14U + 0x18U * (channel))
#define DMA_CHPADDR(channel)            (0x18U + 0x18U * (channel))
#define DMA_CHM0ADDR(channel)           (0x1cU + 0x18U * (channel))
#define DMA_CHM1ADDR(channel)           (0x20U + 0x18U * (channel))
#define DMA_CHFCTL(channel)             (0x24U + 0x18U * (channel))
#define DMA_CHCTL_PRIO(value)           (((uint32_t)(value) & 3U) << 16)
#define DMA_CHCTL_MWIDTH(value)         (((uint32_t)(value) & 3U) << 13)
#define DMA_CHCTL_PWIDTH(value)         (((uint32_t)(value) & 3U) << 11)
#define DMA_CHCTL_MNAGA                 (1U << 10)
#define DMA_CHCTL_CMEN                  (1U << 8)
#define DMA_CHCTL_FTFIE                 (1U << 4)
#define DMA_CHCTL_CHEN                  (1U << 0)
#define DMAMUX_CHCTL(channel)           (4U * (channel))
#define DMAMUX_DMAREQ_ID_MASK           0xffU
#define DMAMUX_REQUEST_ADC0             9U
#define DMAMUX_REQUEST_ADC1             10U
#define DMA_INTF0_FTFIF(channel)        (1U << (5U + 6U * (channel)))
#define DMA_INTC0_CLEAR_MASK(channel)   (0x3dU << (6U * (channel)))

#define MOTOR_IRQ_PRIORITY 2
#define DMA0_Channel0_IRQn 11
#define DMA0_Channel1_IRQn 12
#define ADC0_1_IRQn        18

struct c5_adc_sequence {
    uint32_t adc_base;
    uint8_t routine_rank0;
    uint8_t routine_rank1;
    uint8_t inserted_rank0;
    uint8_t inserted_rank1;
};

struct c5_dma_stream {
    uint8_t axis;
    uint8_t channel;
    uint8_t dmamux_request;
    uint32_t adc_base;
    volatile uint32_t *samples;
    uint32_t ftf_flag;
    uint32_t clear_mask;
};

struct syscfg_timer_route {
    uint16_t cfg0_offset;
    uint32_t tscfg5;
};

struct trigsel_route {
    uint16_t reg_offset;
    uint8_t output_index;
    uint8_t input_id;
};

static const uint32_t pwm_bases[] = {
    TIMER3_BASE, TIMER7_BASE, TIMER1_BASE,
};
static const uint8_t pwm_compare_offsets[8] = {
    TIMER_CH0CV_OFFSET, TIMER_CH0COMV_ADD_OFFSET,
    TIMER_CH1CV_OFFSET, TIMER_CH1COMV_ADD_OFFSET,
    TIMER_CH2CV_OFFSET, TIMER_CH2COMV_ADD_OFFSET,
    TIMER_CH3CV_OFFSET, TIMER_CH3COMV_ADD_OFFSET,
};
static struct c5_mclib_acq acquisitions[3];
static volatile uint32_t dma_samples_y[2] __attribute__((aligned(4)));
static volatile uint32_t dma_samples_z[2] __attribute__((aligned(4)));

static const struct c5_adc_sequence adc_sequences[] = {
    { ADC0_BASE, 8, 4, 5, 9 },
    { ADC1_BASE, 7, 3, 18, 19 },
};
static const struct c5_dma_stream dma_stream_y = {
    .axis = 1, .channel = 0, .dmamux_request = DMAMUX_REQUEST_ADC0,
    .adc_base = ADC0_BASE, .samples = dma_samples_y,
    .ftf_flag = DMA_INTF0_FTFIF(0), .clear_mask = DMA_INTC0_CLEAR_MASK(0),
};
static const struct c5_dma_stream dma_stream_z = {
    .axis = 2, .channel = 1, .dmamux_request = DMAMUX_REQUEST_ADC1,
    .adc_base = ADC1_BASE, .samples = dma_samples_z,
    .ftf_flag = DMA_INTF0_FTFIF(1), .clear_mask = DMA_INTC0_CLEAR_MASK(1),
};

_Static_assert(TIMER_CH0CV_OFFSET == 0x34U
               && TIMER_CH1CV_OFFSET == 0x38U
               && TIMER_CH2CV_OFFSET == 0x3cU
               && TIMER_CH3CV_OFFSET == 0x40U,
               "TIMER channel compare offsets mismatch");
_Static_assert(TIMER_CH0COMV_ADD_OFFSET == 0x64U
               && TIMER_CH1COMV_ADD_OFFSET == 0x68U
               && TIMER_CH2COMV_ADD_OFFSET == 0x6cU
               && TIMER_CH3COMV_ADD_OFFSET == 0x70U,
               "TIMER additional compare offsets mismatch");
_Static_assert(ADC_RSQ7_OFFSET == 0x40U && ADC_RSQ8_OFFSET == 0x44U
               && ADC_ISQ0_OFFSET == 0x48U && ADC_ISQ1_OFFSET == 0x4cU,
               "ADC sequence register offsets mismatch");

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
    return REG32(TIMER22_BASE + TIMER_CNT_OFFSET);
}

void
c5_mainboardgd_pwm_enable(uint8_t axis, uint8_t enable)
{
    volatile uint32_t *chctl2 = reg_at(pwm_base(axis), TIMER_CHCTL2_OFFSET);
    if (enable)
        *chctl2 |= TIMER_CHCTL2_CHANNEL_ENABLE_MASK;
    else
        *chctl2 &= ~TIMER_CHCTL2_CHANNEL_ENABLE_MASK;
}

static void
disable_all_pwm_outputs(void)
{
    uint_fast8_t axis;
    for (axis = 0; axis < 3; axis++)
        REG32(pwm_bases[axis] + TIMER_CHCTL2_OFFSET)
            &= ~TIMER_CHCTL2_CHANNEL_ENABLE_MASK;
}

static uint8_t
control_output_valid(const struct c5_mclib_output *out)
{
    if (out->signs & ~3U)
        return 0;
    uint_fast8_t i;
    for (i = 0; i < 8; i++)
        if (out->compare[i] > TIMER_CAR_20KHZ)
            return 0;
    return 1;
}

static void
write_control_output(uint8_t axis, const struct c5_mclib_output *out)
{
    uint32_t base = pwm_base(axis);
    uint_fast8_t i;
    for (i = 0; i < 8; i++)
        REG32(base + pwm_compare_offsets[i]) = out->compare[i];
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
    uint8_t status = c5_mainboardgd_control(axis, now, ia, ib, &out);
    if (!status)
        return;
    if (status != 1 || !control_output_valid(&out)) {
        disable_all_pwm_outputs();
        shutdown("Invalid mainBoardGD PWM output");
        return;
    }
    write_control_output(axis, &out);
}

void
DMA0_Channel0_IRQHandler(void)
{
    uint32_t status = REG32(DMA0_BASE + DMA_INTF0);
    if (!(status & dma_stream_y.ftf_flag))
        return;
    REG32(DMA0_BASE + DMA_INTC0) = dma_stream_y.clear_mask;
    int16_t raw0 = (int16_t)(uint16_t)dma_stream_y.samples[0];
    int16_t raw1 = (int16_t)(uint16_t)dma_stream_y.samples[1];
    process_sample(dma_stream_y.axis, raw0, raw1);
}
DECL_ARMCM_IRQ(DMA0_Channel0_IRQHandler, DMA0_Channel0_IRQn);

void
DMA0_Channel1_IRQHandler(void)
{
    uint32_t status = REG32(DMA0_BASE + DMA_INTF0);
    if (!(status & dma_stream_z.ftf_flag))
        return;
    REG32(DMA0_BASE + DMA_INTC0) = dma_stream_z.clear_mask;
    int16_t raw0 = (int16_t)(uint16_t)dma_stream_z.samples[0];
    int16_t raw1 = (int16_t)(uint16_t)dma_stream_z.samples[1];
    process_sample(dma_stream_z.axis, raw0, raw1);
}
DECL_ARMCM_IRQ(DMA0_Channel1_IRQHandler, DMA0_Channel1_IRQn);

void
ADC_IRQHandler(void)
{
    uint32_t stat0 = REG32(ADC0_BASE + ADC_STAT_OFFSET);
    if (stat0 & ADC_STAT_EOIC) {
        int16_t raw0 = (int16_t)(uint16_t)REG32(
            ADC0_BASE + ADC_IDATA0_OFFSET);
        int16_t raw1 = (int16_t)(uint16_t)REG32(
            ADC0_BASE + ADC_IDATA0_OFFSET + 4U);
        REG32(ADC0_BASE + ADC_STAT_OFFSET) = ~ADC_STAT_EOIC;
        process_sample(0, raw0, raw1);
    }
    if (REG32(ADC1_BASE + ADC_STAT_OFFSET) & ADC_STAT_EOIC)
        REG32(ADC1_BASE + ADC_STAT_OFFSET) = ~ADC_STAT_EOIC;
}
DECL_ARMCM_IRQ(ADC_IRQHandler, ADC0_1_IRQn);

static void
setup_pwm_timer(uint32_t base)
{
    enable_pclock(base);
    REG32(base + TIMER_CTL0_OFFSET) &= ~TIMER_CTL0_COUNT_MODE_MASK;
    REG32(base + TIMER_PSC_OFFSET) = 0;
    REG32(base + TIMER_CAR_OFFSET) = TIMER_CAR_20KHZ;
    REG32(base + TIMER_SWEVG_OFFSET) = 1U;

    REG32(base + TIMER_CHCTL0_OFFSET) =
        (REG32(base + TIMER_CHCTL0_OFFSET) & 0x0efe8484U) | 0x30006868U;
    REG32(base + TIMER_CHCTL1_OFFSET) =
        (REG32(base + TIMER_CHCTL1_OFFSET) & 0x0efe8484U) | 0x30006868U;
    REG32(base + TIMER_CTL2_OFFSET) |= TIMER_CTL2_CHXCPWMEN_MASK;

    if (base == TIMER7_BASE) {
        REG32(base + TIMER_CHCTL2_OFFSET) &= ~0xffffU;
        REG32(base + TIMER_CTL1_OFFSET) &= ~0xff00U;
        REG32(base + TIMER_CREP0_OFFSET) = 0;
        REG32(base + TIMER_CFG_OFFSET) &= ~TIMER_CFG_CREPSEL;
        REG32(base + TIMER_CCHP_OFFSET) |= TIMER_CCHP_POEN;
    } else {
        REG32(base + TIMER_CHCTL2_OFFSET) &= ~0x3333U;
    }

    uint_fast8_t i;
    for (i = 0; i < 8; i++)
        REG32(base + pwm_compare_offsets[i]) = 0;
}

static void
setup_trigger_timer(uint32_t base, const uint16_t compare[4])
{
    enable_pclock(base);
    REG32(base + TIMER_CTL0_OFFSET) &= ~TIMER_CTL0_COUNT_MODE_MASK;
    REG32(base + TIMER_PSC_OFFSET) = 0;
    REG32(base + TIMER_CAR_OFFSET) = TIMER_CAR_20KHZ;
    REG32(base + TIMER_CHCTL0_OFFSET) =
        (REG32(base + TIMER_CHCTL0_OFFSET) & 0x3efe8484U) | 0x7878U;
    REG32(base + TIMER_CHCTL1_OFFSET) =
        (REG32(base + TIMER_CHCTL1_OFFSET) & 0x3efe8484U) | 0x7878U;
    REG32(base + TIMER_CHCTL2_OFFSET) =
        (REG32(base + TIMER_CHCTL2_OFFSET) & ~0x3333U) | 0x1111U;
    REG32(base + TIMER_CH0CV_OFFSET) = compare[0];
    REG32(base + TIMER_CH1CV_OFFSET) = compare[1];
    REG32(base + TIMER_CH2CV_OFFSET) = compare[2];
    REG32(base + TIMER_CH3CV_OFFSET) = compare[3];
    if (base == TIMER2_BASE) {
        REG32(base + TIMER_SMCFG_OFFSET) |= TIMER_SMCFG_MSM;
        REG32(base + TIMER_CTL1_OFFSET) =
            (REG32(base + TIMER_CTL1_OFFSET) & ~TIMER_CTL1_MMC0_MASK)
            | TIMER_CTL1_MMC0(1);
    }
    REG32(base + TIMER_SWEVG_OFFSET) = 1U;
}

static void
setup_timer_routing(void)
{
    static const struct syscfg_timer_route syscfg_routes[] = {
        { SYSCFG_TIMER0CFG0_OFFSET, SYSCFG_TSCFG5_EVENT_ITI14 },
        { SYSCFG_TIMER1CFG0_OFFSET, SYSCFG_TSCFG5_EVENT_ITI14 },
        { SYSCFG_TIMER3CFG0_OFFSET, SYSCFG_TSCFG5_EVENT_ITI14 },
        { SYSCFG_TIMER7CFG0_OFFSET, SYSCFG_TSCFG5_EVENT_ITI14 },
        { SYSCFG_TIMER4CFG0_OFFSET, SYSCFG_TSCFG5_EVENT_ITI2 },
    };
    static const struct trigsel_route trigsel_routes[] = {
        { TRIGSEL_TIMER7ITI14_OFFSET, 0, TRIGSEL_IN_TIMER2_CH0 },
        { TRIGSEL_ADC0_OFFSET, 0, TRIGSEL_IN_TIMER4_CH2 },
        { TRIGSEL_TIMER3ITI14_OFFSET, 0, TRIGSEL_IN_TIMER2_CH1 },
        { TRIGSEL_ADC0_OFFSET, 1, TRIGSEL_IN_TIMER4_CH3 },
        { TRIGSEL_TIMER1ITI14_OFFSET, 0, TRIGSEL_IN_TIMER2_CH2 },
        { TRIGSEL_ADC1_OFFSET, 0, TRIGSEL_IN_TIMER4_CH0 },
        { TRIGSEL_TIMER0ITI14_OFFSET, 0, TRIGSEL_IN_TIMER2_CH3 },
        { TRIGSEL_ADC1_OFFSET, 1, TRIGSEL_IN_TIMER4_CH1 },
    };

    uint_fast8_t i;
    for (i = 0; i < sizeof(syscfg_routes) / sizeof(syscfg_routes[0]); i++) {
        uint32_t address = SYSCFG_BASE + syscfg_routes[i].cfg0_offset;
        REG32(address) = 0;
        REG32(address + 4U) = 0;
        REG32(address + 8U) = 0;
        REG32(address) = syscfg_routes[i].tscfg5;
    }
    for (i = 0; i < sizeof(trigsel_routes) / sizeof(trigsel_routes[0]); i++) {
        const struct trigsel_route *route = &trigsel_routes[i];
        uint32_t address = TRIGSEL_BASE + route->reg_offset;
        uint32_t value = REG32(address);
        if (value & TRIGSEL_TARGET_LK)
            continue;
        uint32_t shift = 8U * route->output_index;
        value = (value & ~TRIGSEL_INSEL_MASK(route->output_index))
                | ((uint32_t)route->input_id << shift);
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
setup_adc(const struct c5_adc_sequence *adc)
{
    uint32_t base = adc->adc_base;
    REG32(base + ADC_CTL0_OFFSET) =
        (REG32(base + ADC_CTL0_OFFSET) & ~0x03000000U) | 0x180U;
    REG32(base + ADC_CTL1_OFFSET) = 0x10100301U;
    REG32(base + ADC_RSQ0_OFFSET) =
        (REG32(base + ADC_RSQ0_OFFSET) & ~ADC_RSQ0_RL_MASK)
        | ADC_RSQ0_RL(2);
    REG32(base + ADC_RSQ8_OFFSET) =
        (REG32(base + ADC_RSQ8_OFFSET) & ~ADC_RSQ8_RANK0_MASK)
        | ADC_RSQ_RANK(adc->routine_rank0);
    REG32(base + ADC_RSQ7_OFFSET) =
        (REG32(base + ADC_RSQ7_OFFSET) & ~ADC_RSQ7_RANK1_MASK)
        | ADC_RSQ_RANK(adc->routine_rank1);
    REG32(base + ADC_ISQ0_OFFSET) =
        (REG32(base + ADC_ISQ0_OFFSET)
         & ~(ADC_ISQ0_IL_MASK | ADC_ISQ0_RANK1_MASK))
        | ADC_ISQ0_IL(2) | ADC_ISQ0_RANK1(adc->inserted_rank1);
    REG32(base + ADC_ISQ1_OFFSET) =
        (REG32(base + ADC_ISQ1_OFFSET) & ~ADC_ISQ1_RANK0_MASK)
        | ADC_ISQ1_RANK0(adc->inserted_rank0);
    REG32(base + ADC_STAT_OFFSET) = ~ADC_STAT_EOIC;

    udelay(100);
    REG32(base + ADC_CTL1_OFFSET) |= 1U << 27;
    REG32(base + ADC_CTL1_OFFSET) &= ~0x70U;
    REG32(base + ADC_CTL1_OFFSET) |= ADC_CTL1_RSTCLB;
    while (REG32(base + ADC_CTL1_OFFSET) & ADC_CTL1_RSTCLB)
        ;
    REG32(base + ADC_CTL1_OFFSET) |= ADC_CTL1_CLB;
    while (REG32(base + ADC_CTL1_OFFSET) & ADC_CTL1_CLB)
        ;
}

static void
setup_dma_channel(const struct c5_dma_stream *stream)
{
    uint32_t channel = stream->channel;
    REG32(DMA0_BASE + DMA_CHCTL(channel)) = 0;
    REG32(DMA0_BASE + DMA_CHCNT(channel)) = 0;
    REG32(DMA0_BASE + DMA_CHPADDR(channel)) = 0;
    REG32(DMA0_BASE + DMA_CHM0ADDR(channel)) = 0;
    REG32(DMA0_BASE + DMA_CHM1ADDR(channel)) = 0;
    REG32(DMA0_BASE + DMA_CHFCTL(channel)) = 0;
    REG32(DMA0_BASE + DMA_CHPADDR(channel)) =
        stream->adc_base + ADC_RDATA_OFFSET;
    REG32(DMA0_BASE + DMA_CHM0ADDR(channel)) = (uint32_t)stream->samples;
    REG32(DMA0_BASE + DMA_CHCNT(channel)) = 2;

    uint32_t mux = DMAMUX_BASE + DMAMUX_CHCTL(channel);
    REG32(mux) = (REG32(mux) & ~DMAMUX_DMAREQ_ID_MASK)
        | (stream->dmamux_request & DMAMUX_DMAREQ_ID_MASK);
    REG32(DMA0_BASE + DMA_INTC0) = stream->clear_mask;
    REG32(DMA0_BASE + DMA_CHCTL(channel)) =
        DMA_CHCTL_PRIO(3) | DMA_CHCTL_MWIDTH(2) | DMA_CHCTL_PWIDTH(2)
        | DMA_CHCTL_MNAGA | DMA_CHCTL_CMEN | DMA_CHCTL_FTFIE
        | DMA_CHCTL_CHEN;
}

static void
setup_motor_time(void)
{
    enable_pclock(TIMER22_BASE);
    REG32(TIMER22_BASE + TIMER_CTL0_OFFSET) &= ~TIMER_CTL0_COUNT_MODE_MASK;
    REG32(TIMER22_BASE + TIMER_PSC_OFFSET) = 2;
    REG32(TIMER22_BASE + TIMER_CAR_OFFSET) = 0xffffffffU;
    REG32(TIMER22_BASE + TIMER_SWEVG_OFFSET) = 1U;
    REG32(TIMER22_BASE + TIMER_CTL0_OFFSET) |= 1U;
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
    REG32(ADC0_SYNCCTL) =
        (REG32(ADC0_SYNCCTL) & ~0x00ff0000U) | 0x000a0000U;
    setup_adc(&adc_sequences[0]);
    setup_adc(&adc_sequences[1]);

    setup_dma_channel(&dma_stream_y);
    setup_dma_channel(&dma_stream_z);

    armcm_enable_irq(DMA0_Channel0_IRQHandler, DMA0_Channel0_IRQn,
                     MOTOR_IRQ_PRIORITY);
    armcm_enable_irq(DMA0_Channel1_IRQHandler, DMA0_Channel1_IRQn,
                     MOTOR_IRQ_PRIORITY);
    armcm_enable_irq(ADC_IRQHandler, ADC0_1_IRQn, MOTOR_IRQ_PRIORITY);
    REG32(TIMER2_BASE + TIMER_CTL0_OFFSET) |= 1U;
}
DECL_INIT(c5_mainboardgd_hardware_init);

void
c5_mainboardgd_shutdown(void)
{
    irqstatus_t flag = irq_save();
    disable_all_pwm_outputs();
    c5_mainboardgd_enable(0, 0);
    c5_mainboardgd_enable(1, 0);
    c5_mainboardgd_enable(2, 0);
    irq_restore(flag);
}
DECL_SHUTDOWN(c5_mainboardgd_shutdown);
