// Creator 5 mainBoardGD motor peripherals
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdint.h>
#include "board/irq.h" // irq_save
#include "command.h" // shutdown
#include "c5_mainboardgd.h" // c5_mainboardgd_control
#include "c5_mainboardgd_diag.h" // c5_mainboardgd_diag_isr
#include "generic/armcm_boot.h" // armcm_enable_irq
#include "generic/armcm_timer.h" // udelay
#include "internal.h" // enable_pclock
#include "sched.h" // DECL_INIT

#define MOTOR_IRQ_PRIORITY 2
#define TIMER_CAR_20KHZ 14999U

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
    uint8_t timer;
    uint8_t trigger;
};

struct trigsel_route {
    uint8_t target;
    uint8_t input;
};

static const uint32_t pwm_bases[] = {
    TIMER3, TIMER7, TIMER1,
};
static struct c5_mclib_acq acquisitions[3];
static volatile uint32_t dma_samples_y[2] __attribute__((aligned(4)));
static volatile uint32_t dma_samples_z[2] __attribute__((aligned(4)));
#if CONFIG_C5_MAINBOARDGD_DIAGNOSTICS
#define C5_DIAG_DTCM_ADDRESS ((uint32_t)0x20000000U)
#define C5_DIAG_MAGIC ((uint32_t)0x43443544U)
#define C5_DIAG_VERSION ((uint32_t)1U)
#define C5_DIAG_RESET_MASK ((uint32_t)0xfe000000U)

struct c5_mainboardgd_persistent_diag {
    uint32_t magic;
    uint32_t version;
    uint32_t boot_count;
    uint32_t flags;
    uint32_t fault_pc;
    uint32_t fault_lr;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t last_event[C5_MAINBOARDGD_MOTOR_COUNT];
};

static volatile struct c5_mainboardgd_persistent_diag *const persistent_diag =
    (void *)C5_DIAG_DTCM_ADDRESS;
static struct c5_mainboardgd_hw_diag hardware_diag;
static volatile uint8_t persistent_diag_ready;

static void
c5_mainboardgd_diag_boot(void)
{
    uint32_t reset_status = RCU_RSTSCK & C5_DIAG_RESET_MASK;
    hardware_diag.reset_status = reset_status;
    uint8_t valid = !(reset_status & RCU_RSTSCK_PORRSTF)
                    && persistent_diag->magic == C5_DIAG_MAGIC
                    && persistent_diag->version == C5_DIAG_VERSION;
    if (valid) {
        hardware_diag.fault_pc = persistent_diag->fault_pc;
        hardware_diag.fault_lr = persistent_diag->fault_lr;
        hardware_diag.cfsr = persistent_diag->cfsr;
        hardware_diag.hfsr = persistent_diag->hfsr;
        hardware_diag.flags = persistent_diag->flags
                              | C5_MAINBOARDGD_DIAG_PERSIST_VALID;
        uint_fast8_t axis;
        for (axis = 0; axis < C5_MAINBOARDGD_MOTOR_COUNT; axis++)
            hardware_diag.previous_event[axis] =
                persistent_diag->last_event[axis];
    }

    persistent_diag->magic = C5_DIAG_MAGIC;
    persistent_diag->version = C5_DIAG_VERSION;
    persistent_diag->boot_count = valid ? persistent_diag->boot_count + 1U : 1U;
    hardware_diag.boot_count = persistent_diag->boot_count;
    persistent_diag->flags = 0;
    persistent_diag->fault_pc = 0;
    persistent_diag->fault_lr = 0;
    persistent_diag->cfsr = 0;
    persistent_diag->hfsr = 0;
    uint_fast8_t axis;
    for (axis = 0; axis < C5_MAINBOARDGD_MOTOR_COUNT; axis++)
        persistent_diag->last_event[axis] = 0;
    c5_mainboardgd_diag_clear();
    persistent_diag_ready = 1;
    RCU_RSTSCK |= RCU_RSTSCK_RSTFC;
}

void
c5_mainboardgd_hw_diag_snapshot(struct c5_mainboardgd_hw_diag *snapshot)
{
    *snapshot = hardware_diag;
    snapshot->adc_stat0 = ADC_STAT(ADC0);
    snapshot->adc_stat1 = ADC_STAT(ADC1);
    snapshot->dma_intf = DMA_INTF0(DMA0);
}

void
c5_mainboardgd_hw_diag_event(uint8_t axis, uint8_t event)
{
    if (!persistent_diag->last_event[axis])
        persistent_diag->last_event[axis] = event;
}

void
c5_mainboardgd_hw_diag_shutdown(void)
{
    persistent_diag->flags |= C5_MAINBOARDGD_DIAG_SHUTDOWN;
}

static void __attribute__((used, noreturn))
c5_mainboardgd_hardfault(uint32_t *frame)
{
    __disable_irq();
    persistent_diag->magic = C5_DIAG_MAGIC;
    persistent_diag->version = C5_DIAG_VERSION;
    uint32_t flags = C5_MAINBOARDGD_DIAG_HARDFAULT;
    if (persistent_diag_ready)
        flags |= persistent_diag->flags;
    persistent_diag->flags = flags;
    persistent_diag->fault_pc = frame[6];
    persistent_diag->fault_lr = frame[5];
    persistent_diag->cfsr = SCB->CFSR;
    persistent_diag->hfsr = SCB->HFSR;
    __DSB();
    NVIC_SystemReset();
}

void __attribute__((naked))
HardFault_Handler(void)
{
    asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b c5_mainboardgd_hardfault");
}
DECL_ARMCM_IRQ(HardFault_Handler, -13);
#endif

static const struct c5_adc_sequence adc_sequences[] = {
    { ADC0, 8, 4, 5, 9 },
    { ADC1, 7, 3, 18, 19 },
};
static const struct c5_dma_stream dma_stream_y = {
    .axis = 1, .channel = 0, .dmamux_request = DMA_REQUEST_ADC0,
    .adc_base = ADC0, .samples = dma_samples_y,
    .ftf_flag = DMA_FLAG_ADD(DMA_INTF_FTFIF, 0),
    .clear_mask = DMA_FLAG_ADD(DMA_INTC_FEEIFC | DMA_INTC_SDEIFC
                               | DMA_INTC_TAEIFC | DMA_INTC_HTFIFC
                               | DMA_INTC_FTFIFC, 0),
};
static const struct c5_dma_stream dma_stream_z = {
    .axis = 2, .channel = 1, .dmamux_request = DMA_REQUEST_ADC1,
    .adc_base = ADC1, .samples = dma_samples_z,
    .ftf_flag = DMA_FLAG_ADD(DMA_INTF_FTFIF, 1),
    .clear_mask = DMA_FLAG_ADD(DMA_INTC_FEEIFC | DMA_INTC_SDEIFC
                               | DMA_INTC_TAEIFC | DMA_INTC_HTFIFC
                               | DMA_INTC_FTFIFC, 1),
};

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
    return TIMER_CNT(TIMER22);
}

void
c5_mainboardgd_pwm_enable(uint8_t axis, uint8_t enable)
{
    uint32_t base = pwm_base(axis);
    uint32_t channel_enable = TIMER_CHCTL2_CH0EN | TIMER_CHCTL2_CH1EN
                              | TIMER_CHCTL2_CH2EN | TIMER_CHCTL2_CH3EN;
    if (enable)
        TIMER_CHCTL2(base) |= channel_enable;
    else
        TIMER_CHCTL2(base) &= ~channel_enable;
}

static void
disable_all_pwm_outputs(void)
{
    uint32_t channel_enable = TIMER_CHCTL2_CH0EN | TIMER_CHCTL2_CH1EN
                              | TIMER_CHCTL2_CH2EN | TIMER_CHCTL2_CH3EN;
    uint_fast8_t axis;
    for (axis = 0; axis < 3; axis++)
        TIMER_CHCTL2(pwm_bases[axis]) &= ~channel_enable;
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
    TIMER_CH0CV(base) = out->compare[0];
    TIMER_CH0COMV_ADD(base) = out->compare[1];
    TIMER_CH1CV(base) = out->compare[2];
    TIMER_CH1COMV_ADD(base) = out->compare[3];
    TIMER_CH2CV(base) = out->compare[4];
    TIMER_CH2COMV_ADD(base) = out->compare[5];
    TIMER_CH3CV(base) = out->compare[6];
    TIMER_CH3COMV_ADD(base) = out->compare[7];
    c5_mclib_acq_polarity(&acquisitions[axis], out->signs);
}

static void
process_sample(uint8_t axis, int16_t raw0, int16_t raw1)
{
    float ia, ib;
    if (!c5_mclib_acquire(&acquisitions[axis], raw0, raw1, &ia, &ib)) {
#if CONFIG_C5_MAINBOARDGD_DIAGNOSTICS
        c5_mainboardgd_diag_idle(axis);
#endif
        return;
    }

    struct c5_mclib_output out;
    uint32_t now = c5_mainboardgd_motor_time();
    uint8_t status = c5_mainboardgd_control(axis, now, ia, ib, &out);
    if (!status) {
#if CONFIG_C5_MAINBOARDGD_DIAGNOSTICS
        c5_mainboardgd_diag_idle(axis);
#endif
        return;
    }
    if (status != 1 || !control_output_valid(&out)) {
#if CONFIG_C5_MAINBOARDGD_DIAGNOSTICS
        uint8_t event = status != 1 ? C5_DIAG_EVENT_MCLIB_OUTPUT
                        : out.signs & ~3U ? C5_DIAG_EVENT_SIGNS
                                         : C5_DIAG_EVENT_COMPARE;
        c5_mainboardgd_diag_event(axis, event);
        c5_mainboardgd_hw_diag_event(axis, event);
#endif
        disable_all_pwm_outputs();
        shutdown("Invalid mainBoardGD PWM output");
        return;
    }
    write_control_output(axis, &out);
}

static void
process_dma(const struct c5_dma_stream *stream)
{
    uint32_t status = DMA_INTF0(DMA0);
#if CONFIG_C5_MAINBOARDGD_DIAGNOSTICS
    uint32_t started = c5_mainboardgd_motor_time();
    uint32_t error_mask = DMA_FLAG_ADD(DMA_INTF_FEEIF | DMA_INTF_SDEIF
                                       | DMA_INTF_TAEIF, stream->channel);
    uint8_t dma_error = !!(status & error_mask);
    uint8_t adc_error = !!(ADC_STAT(stream->adc_base) & ADC_STAT_ROVF);
    c5_mainboardgd_diag_isr(stream->axis, dma_error || adc_error);
    if (dma_error || adc_error) {
        uint8_t event = adc_error ? C5_DIAG_EVENT_ADC_OVERRUN
                                  : C5_DIAG_EVENT_DMA;
        c5_mainboardgd_diag_event(stream->axis, event);
        c5_mainboardgd_hw_diag_event(stream->axis, event);
    }
#endif
    if (!(status & stream->ftf_flag))
        return;
    DMA_INTC0(DMA0) = stream->clear_mask;
    int16_t raw0 = (int16_t)(uint16_t)stream->samples[0];
    int16_t raw1 = (int16_t)(uint16_t)stream->samples[1];
    process_sample(stream->axis, raw0, raw1);
#if CONFIG_C5_MAINBOARDGD_DIAGNOSTICS
    c5_mainboardgd_diag_timing(stream->axis, started,
                               c5_mainboardgd_motor_time());
#endif
}

void
DMA0_Channel0_IRQHandler(void)
{
    process_dma(&dma_stream_y);
}
DECL_ARMCM_IRQ(DMA0_Channel0_IRQHandler, DMA0_Channel0_IRQn);

void
DMA0_Channel1_IRQHandler(void)
{
    process_dma(&dma_stream_z);
}
DECL_ARMCM_IRQ(DMA0_Channel1_IRQHandler, DMA0_Channel1_IRQn);

void
ADC_IRQHandler(void)
{
    uint32_t stat0 = ADC_STAT(ADC0);
    if (stat0 & ADC_STAT_EOIC) {
#if CONFIG_C5_MAINBOARDGD_DIAGNOSTICS
        uint32_t started = c5_mainboardgd_motor_time();
        c5_mainboardgd_diag_isr(0, 0);
#endif
        int16_t raw0 = (int16_t)(uint16_t)ADC_IDATA0(ADC0);
        int16_t raw1 = (int16_t)(uint16_t)ADC_IDATA1(ADC0);
        ADC_STAT(ADC0) = ~ADC_STAT_EOIC;
        process_sample(0, raw0, raw1);
#if CONFIG_C5_MAINBOARDGD_DIAGNOSTICS
        c5_mainboardgd_diag_timing(0, started,
                                   c5_mainboardgd_motor_time());
#endif
    }
    if (ADC_STAT(ADC1) & ADC_STAT_EOIC)
        ADC_STAT(ADC1) = ~ADC_STAT_EOIC;
}
DECL_ARMCM_IRQ(ADC_IRQHandler, ADC0_1_IRQn);

static void
setup_pwm_timer(uint32_t base)
{
    enable_pclock(base);
    TIMER_CTL0(base) &= ~(TIMER_CTL0_DIR | TIMER_CTL0_CAM | TIMER_CTL0_CKDIV);
    TIMER_PSC(base) = 0;
    TIMER_CAR(base) = TIMER_CAR_20KHZ;
    TIMER_SWEVG(base) = TIMER_SWEVG_UPG;

    uint32_t ch01_mask = TIMER_CHCTL0_CH0MS | TIMER_CHCTL0_CH0COMSEN
                         | TIMER_CHCTL0_CH0COMCTL
                         | TIMER_CHCTL0_CH1MS | TIMER_CHCTL0_CH1COMSEN
                         | TIMER_CHCTL0_CH1COMCTL
                         | TIMER_CHCTL0_CH0COMADDSEN
                         | TIMER_CHCTL0_CH1COMADDSEN;
    uint32_t ch01_value = TIMER_CHCTL0_CH0COMSEN
                          | TIMER_CHCTL0_CH0COMCTL_VALUE(TIMER_OC_MODE_PWM0)
                          | TIMER_CHCTL0_CH1COMSEN
                          | TIMER_CHCTL0_CH1COMCTL_VALUE(TIMER_OC_MODE_PWM0)
                          | TIMER_CHCTL0_CH0COMADDSEN
                          | TIMER_CHCTL0_CH1COMADDSEN;
    uint32_t ch23_mask = TIMER_CHCTL1_CH2MS | TIMER_CHCTL1_CH2COMSEN
                         | TIMER_CHCTL1_CH2COMCTL
                         | TIMER_CHCTL1_CH3MS | TIMER_CHCTL1_CH3COMSEN
                         | TIMER_CHCTL1_CH3COMCTL
                         | TIMER_CHCTL1_CH2COMADDSEN
                         | TIMER_CHCTL1_CH3COMADDSEN;
    uint32_t ch23_value = TIMER_CHCTL1_CH2COMSEN
                          | TIMER_CHCTL1_CH2COMCTL_VALUE(TIMER_OC_MODE_PWM0)
                          | TIMER_CHCTL1_CH3COMSEN
                          | TIMER_CHCTL1_CH3COMCTL_VALUE(TIMER_OC_MODE_PWM0)
                          | TIMER_CHCTL1_CH2COMADDSEN
                          | TIMER_CHCTL1_CH3COMADDSEN;
    TIMER_CHCTL0(base) = (TIMER_CHCTL0(base) & ~ch01_mask) | ch01_value;
    TIMER_CHCTL1(base) = (TIMER_CHCTL1(base) & ~ch23_mask) | ch23_value;
    TIMER_CTL2(base) |= TIMER_CTL2_CH0CPWMEN | TIMER_CTL2_CH1CPWMEN
                        | TIMER_CTL2_CH2CPWMEN | TIMER_CTL2_CH3CPWMEN;

    uint32_t channel_enable_polarity = TIMER_CHCTL2_CH0EN | TIMER_CHCTL2_CH0P
                                       | TIMER_CHCTL2_CH1EN | TIMER_CHCTL2_CH1P
                                       | TIMER_CHCTL2_CH2EN | TIMER_CHCTL2_CH2P
                                       | TIMER_CHCTL2_CH3EN | TIMER_CHCTL2_CH3P;
    if (base == TIMER7) {
        uint32_t all_channel_control =
            TIMER_CHCTL2_CH0EN | TIMER_CHCTL2_CH0P
            | TIMER_CHCTL2_CH0NEN | TIMER_CHCTL2_CH0NP
            | TIMER_CHCTL2_CH1EN | TIMER_CHCTL2_CH1P
            | TIMER_CHCTL2_CH1NEN | TIMER_CHCTL2_CH1NP
            | TIMER_CHCTL2_CH2EN | TIMER_CHCTL2_CH2P
            | TIMER_CHCTL2_CH2NEN | TIMER_CHCTL2_CH2NP
            | TIMER_CHCTL2_CH3EN | TIMER_CHCTL2_CH3P
            | TIMER_CHCTL2_CH3NEN | TIMER_CHCTL2_CH3NP;
        TIMER_CHCTL2(base) &= ~all_channel_control;
        TIMER_CTL1(base) &= ~(TIMER_CTL1_ISO0 | TIMER_CTL1_ISO0N
                              | TIMER_CTL1_ISO1 | TIMER_CTL1_ISO1N
                              | TIMER_CTL1_ISO2 | TIMER_CTL1_ISO2N
                              | TIMER_CTL1_ISO3 | TIMER_CTL1_ISO3N);
        TIMER_CREP0(base) = 0;
        TIMER_CFG(base) &= ~TIMER_CFG_CREPSEL;
        TIMER_CCHP(base) |= TIMER_CCHP_POEN;
    } else {
        TIMER_CHCTL2(base) &= ~channel_enable_polarity;
    }

    TIMER_CH0CV(base) = 0;
    TIMER_CH0COMV_ADD(base) = 0;
    TIMER_CH1CV(base) = 0;
    TIMER_CH1COMV_ADD(base) = 0;
    TIMER_CH2CV(base) = 0;
    TIMER_CH2COMV_ADD(base) = 0;
    TIMER_CH3CV(base) = 0;
    TIMER_CH3COMV_ADD(base) = 0;
}

static void
setup_trigger_timer(uint32_t base, const uint16_t compare[4])
{
    enable_pclock(base);
    TIMER_CTL0(base) &= ~(TIMER_CTL0_DIR | TIMER_CTL0_CAM | TIMER_CTL0_CKDIV);
    TIMER_PSC(base) = 0;
    TIMER_CAR(base) = TIMER_CAR_20KHZ;

    uint32_t ch01_mask = TIMER_CHCTL0_CH0MS | TIMER_CHCTL0_CH0COMSEN
                         | TIMER_CHCTL0_CH0COMCTL
                         | TIMER_CHCTL0_CH1MS | TIMER_CHCTL0_CH1COMSEN
                         | TIMER_CHCTL0_CH1COMCTL;
    uint32_t ch01_value = TIMER_CHCTL0_CH0COMSEN
                          | TIMER_CHCTL0_CH0COMCTL_VALUE(TIMER_OC_MODE_PWM1)
                          | TIMER_CHCTL0_CH1COMSEN
                          | TIMER_CHCTL0_CH1COMCTL_VALUE(TIMER_OC_MODE_PWM1);
    uint32_t ch23_mask = TIMER_CHCTL1_CH2MS | TIMER_CHCTL1_CH2COMSEN
                         | TIMER_CHCTL1_CH2COMCTL
                         | TIMER_CHCTL1_CH3MS | TIMER_CHCTL1_CH3COMSEN
                         | TIMER_CHCTL1_CH3COMCTL;
    uint32_t ch23_value = TIMER_CHCTL1_CH2COMSEN
                          | TIMER_CHCTL1_CH2COMCTL_VALUE(TIMER_OC_MODE_PWM1)
                          | TIMER_CHCTL1_CH3COMSEN
                          | TIMER_CHCTL1_CH3COMCTL_VALUE(TIMER_OC_MODE_PWM1);
    TIMER_CHCTL0(base) = (TIMER_CHCTL0(base) & ~ch01_mask) | ch01_value;
    TIMER_CHCTL1(base) = (TIMER_CHCTL1(base) & ~ch23_mask) | ch23_value;

    uint32_t channel_enable_polarity = TIMER_CHCTL2_CH0EN | TIMER_CHCTL2_CH0P
                                       | TIMER_CHCTL2_CH1EN | TIMER_CHCTL2_CH1P
                                       | TIMER_CHCTL2_CH2EN | TIMER_CHCTL2_CH2P
                                       | TIMER_CHCTL2_CH3EN | TIMER_CHCTL2_CH3P;
    uint32_t channel_enable = TIMER_CHCTL2_CH0EN | TIMER_CHCTL2_CH1EN
                              | TIMER_CHCTL2_CH2EN | TIMER_CHCTL2_CH3EN;
    TIMER_CHCTL2(base) =
        (TIMER_CHCTL2(base) & ~channel_enable_polarity) | channel_enable;
    TIMER_CH0CV(base) = compare[0];
    TIMER_CH1CV(base) = compare[1];
    TIMER_CH2CV(base) = compare[2];
    TIMER_CH3CV(base) = compare[3];
    if (base == TIMER2) {
        TIMER_SMCFG(base) |= TIMER_SMCFG_MSM;
        TIMER_CTL1(base) = (TIMER_CTL1(base) & ~TIMER_CTL1_MMC0)
                           | TIMER_TRI_OUT0_SRC_ENABLE;
    }
    TIMER_SWEVG(base) = TIMER_SWEVG_UPG;
}

static void
setup_timer_routing(void)
{
    static const struct syscfg_timer_route syscfg_routes[] = {
        { SYSCFG_TIMER0, TIMER_SMCFG_TRGSEL_ITI14 },
        { SYSCFG_TIMER1, TIMER_SMCFG_TRGSEL_ITI14 },
        { SYSCFG_TIMER3, TIMER_SMCFG_TRGSEL_ITI14 },
        { SYSCFG_TIMER7, TIMER_SMCFG_TRGSEL_ITI14 },
        { SYSCFG_TIMER4, TIMER_SMCFG_TRGSEL_ITI2 },
    };
    static const struct trigsel_route trigsel_routes[] = {
        { TRIGSEL_OUTPUT_TIMER7_ITI14, TRIGSEL_INPUT_TIMER2_CH0 },
        { TRIGSEL_OUTPUT_ADC0_ROUTRG, TRIGSEL_INPUT_TIMER4_CH2 },
        { TRIGSEL_OUTPUT_TIMER3_ITI14, TRIGSEL_INPUT_TIMER2_CH1 },
        { TRIGSEL_OUTPUT_ADC0_INSTRG, TRIGSEL_INPUT_TIMER4_CH3 },
        { TRIGSEL_OUTPUT_TIMER1_ITI14, TRIGSEL_INPUT_TIMER2_CH2 },
        { TRIGSEL_OUTPUT_ADC1_ROUTRG, TRIGSEL_INPUT_TIMER4_CH0 },
        { TRIGSEL_OUTPUT_TIMER0_ITI14, TRIGSEL_INPUT_TIMER2_CH3 },
        { TRIGSEL_OUTPUT_ADC1_INSTRG, TRIGSEL_INPUT_TIMER4_CH1 },
    };

    uint_fast8_t i;
    for (i = 0; i < sizeof(syscfg_routes) / sizeof(syscfg_routes[0]); i++) {
        uint8_t timer = syscfg_routes[i].timer;
        SYSCFG_TIMERCFG0(timer) = 0;
        SYSCFG_TIMERCFG1(timer) = 0;
        SYSCFG_TIMERCFG2(timer) = 0;
        SYSCFG_TIMERCFG0(timer) =
            SYSCFG_TIMERCFG_TSCFG5_VALUE(syscfg_routes[i].trigger);
    }
    for (i = 0; i < sizeof(trigsel_routes) / sizeof(trigsel_routes[0]); i++) {
        const struct trigsel_route *route = &trigsel_routes[i];
        uint32_t value = TRIGSEL_TARGET_REG(route->target);
        if (value & TRIGSEL_TARGET_LK)
            continue;
        uint32_t shift = TRIGSEL_TARGET_PERIPH_SHIFT(route->target);
        value = (value & ~TRIGSEL_TARGET_PERIPH_MASK(route->target))
                | ((uint32_t)route->input << shift);
        TRIGSEL_TARGET_REG(route->target) = value;
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
    ADC_CTL0(base) = (ADC_CTL0(base) & ~ADC_CTL0_DRES)
                     | ADC_CTL0_EOICIE | ADC_CTL0_SM;
    ADC_CTL1(base) = ADC_CTL1_ADCON | ADC_CTL1_DMA | ADC_CTL1_DDM
                     | ((EXTERNAL_TRIGGER_RISING << INSERTED_TRIGGER_MODE)
                        & ADC_CTL1_ETMIC)
                     | ((EXTERNAL_TRIGGER_RISING << ROUTINE_TRIGGER_MODE)
                        & ADC_CTL1_ETMRC);
    ADC_RSQ0(base) = (ADC_RSQ0(base) & ~ADC_RSQ0_RL) | RSQ0_RL(2U - 1U);
    ADC_RSQ8(base) =
        (ADC_RSQ8(base) & ~(ADC_RSQX_RSQN | ADC_RSQX_RSMPN))
        | ((uint32_t)adc->routine_rank0 & ADC_RSQX_RSQN);
    ADC_RSQ7(base) =
        (ADC_RSQ7(base) & ~(ADC_RSQX_RSQN | ADC_RSQX_RSMPN))
        | ((uint32_t)adc->routine_rank1 & ADC_RSQX_RSQN);
    ADC_ISQ0(base) =
        (ADC_ISQ0(base)
         & ~(ADC_ISQ0_IL | ADC_ISQX_ISQN | ADC_ISQX_ISMPN))
        | ISQ0_IL(2U - 1U) | ((uint32_t)adc->inserted_rank1
                              & ADC_ISQX_ISQN);
    uint32_t inserted_rank0_mask =
        (ADC_ISQX_ISQN | ADC_ISQX_ISMPN)
        << ADC_INSERTED_CHANNEL_SHIFT_LENGTH;
    ADC_ISQ1(base) =
        (ADC_ISQ1(base) & ~inserted_rank0_mask)
        | ((uint32_t)adc->inserted_rank0
           << ADC_INSERTED_CHANNEL_SHIFT_LENGTH);
    ADC_STAT(base) = ~ADC_STAT_EOIC;

    udelay(100);
    ADC_CTL1(base) |= ADC_CTL1_CALMOD;
    ADC_CTL1(base) &= ~ADC_CTL1_CALNUM;
    ADC_CTL1(base) |= ADC_CTL1_RSTCLB;
    while (ADC_CTL1(base) & ADC_CTL1_RSTCLB)
        ;
    ADC_CTL1(base) |= ADC_CTL1_CLB;
    while (ADC_CTL1(base) & ADC_CTL1_CLB)
        ;
}

static void
setup_dma_channel(const struct c5_dma_stream *stream)
{
    uint32_t channel = stream->channel;
    DMA_CHCTL(DMA0, channel) = 0;
    DMA_CHCNT(DMA0, channel) = 0;
    DMA_CHPADDR(DMA0, channel) = 0;
    DMA_CHM0ADDR(DMA0, channel) = 0;
    DMA_CHM1ADDR(DMA0, channel) = 0;
    DMA_CHFCTL(DMA0, channel) = 0;
    DMA_CHPADDR(DMA0, channel) = (uint32_t)&ADC_RDATA(stream->adc_base);
    DMA_CHM0ADDR(DMA0, channel) = (uint32_t)stream->samples;
    DMA_CHCNT(DMA0, channel) = 2;

    DMAMUX_RM_CHXCFG(channel) =
        (DMAMUX_RM_CHXCFG(channel) & ~DMAMUX_RM_CHXCFG_MUXID)
        | (stream->dmamux_request & DMAMUX_RM_CHXCFG_MUXID);
    DMA_INTC0(DMA0) = stream->clear_mask;
    DMA_CHCTL(DMA0, channel) =
        DMA_PRIORITY_ULTRA_HIGH | DMA_MEMORY_WIDTH_32BIT
        | DMA_PERIPH_WIDTH_32BIT | DMA_CHXCTL_MNAGA | DMA_CHXCTL_CMEN
        | DMA_CHXCTL_FTFIE | DMA_CHXCTL_CHEN;
}

static void
setup_motor_time(void)
{
    enable_pclock(TIMER22);
    TIMER_CTL0(TIMER22) &=
        ~(TIMER_CTL0_DIR | TIMER_CTL0_CAM | TIMER_CTL0_CKDIV);
    TIMER_PSC(TIMER22) = 2;
    TIMER_CAR(TIMER22) = 0xffffffffU;
    TIMER_SWEVG(TIMER22) = TIMER_SWEVG_UPG;
    TIMER_CTL0(TIMER22) |= TIMER_CTL0_CEN;
}

void
c5_mainboardgd_hardware_init(void)
{
#if CONFIG_C5_MAINBOARDGD_DIAGNOSTICS
    c5_mainboardgd_diag_boot();
#endif
    c5_mainboardgd_init_motors();
    uint_fast8_t axis;
    for (axis = 0; axis < 3; axis++)
        c5_mclib_acq_init(&acquisitions[axis]);

    RCU_AHB1EN |= RCU_AHB1EN_DMA0EN | RCU_AHB1EN_DMAMUXEN;
    RCU_APB2EN |= RCU_APB2EN_TRGSELEN;
    RCU_APB4EN |= RCU_APB4EN_SYSCFGEN;

    setup_motor_gpio();
    setup_pwm_timer(TIMER3);
    setup_pwm_timer(TIMER7);
    setup_pwm_timer(TIMER1);

    static const uint16_t timer2_compare[4] = { 1000, 4750, 8500, 12250 };
    static const uint16_t timer4_compare[4] = { 1120, 4870, 8620, 12370 };
    setup_trigger_timer(TIMER2, timer2_compare);
    setup_trigger_timer(TIMER4, timer4_compare);
    setup_timer_routing();
    setup_motor_time();

    enable_pclock(ADC0);
    enable_pclock(ADC1);
    ADC_SYNCCTL(ADC0) =
        (ADC_SYNCCTL(ADC0) & ~(ADC_SYNCCTL_ADCSCK | ADC_SYNCCTL_ADCCK))
        | ADC_CLK_SYNC_HCLK_DIV6;
    setup_adc(&adc_sequences[0]);
    setup_adc(&adc_sequences[1]);

    setup_dma_channel(&dma_stream_y);
    setup_dma_channel(&dma_stream_z);

    armcm_enable_irq(DMA0_Channel0_IRQHandler, DMA0_Channel0_IRQn,
                     MOTOR_IRQ_PRIORITY);
    armcm_enable_irq(DMA0_Channel1_IRQHandler, DMA0_Channel1_IRQn,
                     MOTOR_IRQ_PRIORITY);
    armcm_enable_irq(ADC_IRQHandler, ADC0_1_IRQn, MOTOR_IRQ_PRIORITY);
    TIMER_CTL0(TIMER2) |= TIMER_CTL0_CEN;
}
DECL_INIT(c5_mainboardgd_hardware_init);

void
c5_mainboardgd_shutdown(void)
{
    irqstatus_t flag = irq_save();
#if CONFIG_C5_MAINBOARDGD_DIAGNOSTICS
    c5_mainboardgd_hw_diag_shutdown();
#endif
    disable_all_pwm_outputs();
    c5_mainboardgd_enable(0, 0);
    c5_mainboardgd_enable(1, 0);
    c5_mainboardgd_enable(2, 0);
    irq_restore(flag);
}
DECL_SHUTDOWN(c5_mainboardgd_shutdown);
