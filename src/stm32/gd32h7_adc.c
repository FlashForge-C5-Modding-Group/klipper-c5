// ADC2 polling support for GD32H737
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <limits.h>
#include "board/irq.h" // irq_save
#include "board/misc.h" // timer_from_us
#include "command.h" // shutdown
#include "generic/armcm_timer.h" // udelay
#include "gpio.h" // gpio_adc_setup
#include "internal.h" // GPIO
#include "sched.h" // shutdown

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define ADC_STAT           REG32(ADC2_BASE + 0x00)
#define ADC_CTL0           REG32(ADC2_BASE + 0x04)
#define ADC_CTL1           REG32(ADC2_BASE + 0x08)
#define ADC_RSQ8           REG32(ADC2_BASE + 0x44)
#define ADC_RDATA          REG32(ADC2_BASE + 0x64)
#define ADC_SYNCCTL        REG32(ADC2_BASE + 0x304)

#define ADC_STAT_EOC       (1U << 1)
#define ADC_CTL1_ADON      (1U << 0)
#define ADC_CTL1_RSTCLB    (1U << 3)
#define ADC_CTL1_CLB       (1U << 2)
#define ADC_CTL1_DAL       (1U << 11)
#define ADC_CTL1_SWSTART   (1U << 30)
#define ADC_RESOLUTION_Msk (3U << 24)
#define ADC_ADCSCK_Msk     (0x0fU << 16)

DECL_CONSTANT("ADC_MAX", 4095);

enum adc_state {
    ADC_IDLE,
    ADC_ACTIVE,
    ADC_DISCARD_PENDING,
};

static enum adc_state adc_state;
static uint32_t active_token;
static uint32_t owner_sequence;

static uint32_t
pin_to_channel(uint32_t pin)
{
    if (pin == GPIO('C', 0))
        return 10;
    if (pin == GPIO('C', 1))
        return 11;
    if (pin == GPIO('C', 3))
        return 1;
    shutdown("Not a valid ADC pin");
    return 0;
}

static void
adc_init(void)
{
    if (is_enabled_pclock(ADC2_BASE))
        return;
    enable_pclock(ADC2_BASE);
    ADC_SYNCCTL = (ADC_SYNCCTL & ~ADC_ADCSCK_Msk) | (9U << 16);
    ADC_CTL0 &= ~ADC_RESOLUTION_Msk;
    ADC_CTL1 = (ADC_CTL1 & ~ADC_CTL1_DAL) | ADC_CTL1_ADON;
    udelay(1);
    ADC_CTL1 |= ADC_CTL1_RSTCLB;
    while (ADC_CTL1 & ADC_CTL1_RSTCLB)
        ;
    ADC_CTL1 |= ADC_CTL1_CLB;
    while (ADC_CTL1 & ADC_CTL1_CLB)
        ;
}

struct gpio_adc
gpio_adc_setup(uint32_t pin)
{
    uint32_t channel = pin_to_channel(pin);
    adc_init();
    if (pin != GPIO('C', 3))
        gpio_peripheral(pin, GPIO_ANALOG, 0);

    irqstatus_t flag = irq_save();
    if (owner_sequence == (UINT32_MAX >> 5)) {
        irq_restore(flag);
        shutdown("ADC owner token overflow");
    }
    uint32_t token = (++owner_sequence << 5) | channel;
    irq_restore(flag);
    return (struct gpio_adc){ .adc = (void *)ADC2_BASE, .chan = token };
}

static void
adc_drain_completion(void)
{
    (void)ADC_RDATA;
}

uint32_t
gpio_adc_sample(struct gpio_adc g)
{
    irqstatus_t flag = irq_save();
    if (g.adc != (void *)ADC2_BASE) {
        irq_restore(flag);
        shutdown("Not a valid ADC pin");
    }

    if (adc_state == ADC_DISCARD_PENDING) {
        if (!(ADC_STAT & ADC_STAT_EOC)) {
            irq_restore(flag);
            return timer_from_us(20);
        }
        adc_drain_completion();
        adc_state = ADC_IDLE;
        active_token = 0;
    }

    if (adc_state == ADC_ACTIVE) {
        uint8_t ready = active_token == g.chan && (ADC_STAT & ADC_STAT_EOC);
        irq_restore(flag);
        return ready ? 0 : timer_from_us(20);
    }

    if (ADC_STAT & ADC_STAT_EOC)
        adc_drain_completion();
    active_token = g.chan;
    adc_state = ADC_ACTIVE;
    ADC_RSQ8 = (638U << 5) | (g.chan & 31U);
    ADC_CTL1 |= ADC_CTL1_SWSTART;
    irq_restore(flag);
    return timer_from_us(20);
}

uint16_t
gpio_adc_read(struct gpio_adc g)
{
    irqstatus_t flag = irq_save();
    if (g.adc != (void *)ADC2_BASE || adc_state != ADC_ACTIVE
        || active_token != g.chan || !(ADC_STAT & ADC_STAT_EOC)) {
        irq_restore(flag);
        return 0;
    }
    uint16_t value = ADC_RDATA;
    adc_state = ADC_IDLE;
    active_token = 0;
    irq_restore(flag);
    return value;
}

void
gpio_adc_cancel_sample(struct gpio_adc g)
{
    irqstatus_t flag = irq_save();
    if (g.adc == (void *)ADC2_BASE && adc_state == ADC_ACTIVE
        && active_token == g.chan) {
        if (ADC_STAT & ADC_STAT_EOC) {
            adc_drain_completion();
            adc_state = ADC_IDLE;
            active_token = 0;
        } else {
            adc_state = ADC_DISCARD_PENDING;
        }
    }
    irq_restore(flag);
}
