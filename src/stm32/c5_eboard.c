// Creator 5 eBoard private acquisition and TMC transport
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdint.h> // uintptr_t
#include "autoconf.h" // CONFIG_C5_EBOARD
#include "board/armcm_boot.h" // armcm_enable_irq
#include "c5_eboard.h" // c5_eboard_capture
#include "internal.h" // enable_pclock
#include "sched.h" // DECL_INIT


#define N32_RCC_CFG2_SYSCLK_TIM18 0x20000000u
#define N32_DMA1_CH5_TC 0x00020000u
#define N32_DMA1_CH5_ALL 0x000f0000u
#define N32_DMA1_CH2_TC 0x00000020u
#define N32_DMA1_CH3_TC 0x00000200u

static volatile uint32_t dma_snapshot;
static uint32_t previous_snapshot, poll_divider;
static uint8_t tmc_request[4] = { 0x05, 0x00, 0x41, 0xcf };
static volatile uint8_t tmc_response[15];

static volatile uint32_t *
n32_dma_channel_selector(DMA_Channel_TypeDef *channel)
{
    return (volatile uint32_t *)((uintptr_t)channel + 0x10);
}

static volatile uint32_t *
n32_dma1_channel_map_enable(void)
{
    return (volatile uint32_t *)(DMA1_BASE + 0xa8);
}

static volatile uint32_t *
n32_rcc_cfg2(void)
{
    return (volatile uint32_t *)(RCC_BASE + 0x2c);
}

void
DMA1_Channel5_IRQHandler(void)
{
    if (!(DMA1->ISR & N32_DMA1_CH5_TC))
        return;
    DMA1->IFCR = N32_DMA1_CH5_TC;
    uint32_t snapshot = dma_snapshot;
    uint16_t interval = snapshot - previous_snapshot;
    previous_snapshot = snapshot;
    c5_eboard_capture(interval);
}

void
DMA1_Channel2_IRQHandler(void)
{
    if (!(DMA1->ISR & N32_DMA1_CH2_TC))
        return;
    DMA1->IFCR = N32_DMA1_CH2_TC;
    DMA1_Channel2->CCR &= ~1u;
}

void
DMA1_Channel3_IRQHandler(void)
{
    if (DMA1->ISR & N32_DMA1_CH3_TC)
        DMA1->IFCR = N32_DMA1_CH3_TC;
}

void
TIM4_IRQHandler(void)
{
    if (!(TIM4->SR & TIM_SR_UIF))
        return;
    TIM4->SR = ~TIM_SR_UIF;
    if (++poll_divider != 2)
        return;
    poll_divider = 0;
    DMA1_Channel2->CCR &= ~1u;
    DMA1_Channel2->CNDTR = sizeof(tmc_request);
    DMA1_Channel2->CCR |= 1u;
}

void
USART3_IRQHandler(void)
{
    uint32_t sr = USART3->SR;
    if (!(USART3->CR1 & USART_CR1_IDLEIE) || !(sr & USART_SR_IDLE))
        return;
    DMA1_Channel3->CCR &= ~1u;
    uint16_t remaining = DMA1_Channel3->CNDTR;
    c5_eboard_pa_receive(tmc_response, remaining);
    DMA1_Channel3->CNDTR = sizeof(tmc_response);
    (void)USART3->SR;
    (void)USART3->DR;
    DMA1_Channel3->CCR |= 1u;
}

void
c5_eboard_set_pa_mode(uint8_t active)
{
    if (active) {
        TIM4->CR1 |= TIM_CR1_CEN;
        DMA1_Channel5->CCR &= ~1u;
        TIM8->CR1 &= ~TIM_CR1_CEN;
        TIM1->CR1 &= ~TIM_CR1_CEN;
    } else {
        TIM4->CR1 &= ~TIM_CR1_CEN;
        DMA1_Channel5->CCR |= 1u;
        TIM8->CR1 |= TIM_CR1_CEN;
        TIM1->CR1 |= TIM_CR1_CEN;
    }
}

static void
c5_tmc_boot_write(const uint8_t *data, uint_fast8_t length)
{
    for (uint_fast8_t i = 0; i < length; i++) {
        USART3->SR = ~USART_SR_TC;
        USART3->DR = data[i];
        while (!(USART3->SR & USART_SR_TC))
            ;
    }
}

void
c5_eboard_hardware_init(void)
{
    gpio_peripheral(GPIO('B', 8), GPIO_OUTPUT, 0);
    GPIOB->BSRR = GPIO2BIT(GPIO('B', 8));

    *n32_rcc_cfg2() |= N32_RCC_CFG2_SYSCLK_TIM18;
    enable_pclock(TIM8_BASE);
    TIM8->PSC = 0;
    TIM8->ARR = 0xffff;
    TIM8->CNT = 0;
    TIM8->EGR = TIM_EGR_UG;
    TIM8->CR1 = TIM_CR1_CEN;

    gpio_peripheral(GPIO('A', 12), GPIO_INPUT, 0);
    enable_pclock(TIM1_BASE);
    TIM1->PSC = 0;
    TIM1->ARR = 499;
    TIM1->CNT = 0;
    TIM1->RCR = 0;
    TIM1->SMCR = TIM_SMCR_ECE;
    TIM1->DIER = TIM_DIER_UDE;

    enable_pclock(DMA1_BASE);
    *n32_dma1_channel_map_enable() = 1;
    DMA1_Channel5->CCR = 0;
    DMA1_Channel5->CPAR = (uint32_t)(uintptr_t)&TIM8->CNT;
    DMA1_Channel5->CMAR = (uint32_t)(uintptr_t)&dma_snapshot;
    DMA1_Channel5->CNDTR = 1;
    *n32_dma_channel_selector(DMA1_Channel5) = 0x18;
    DMA1_Channel5->CCR = 0x2920;
    DMA1->IFCR = N32_DMA1_CH5_ALL;
    armcm_enable_irq(DMA1_Channel5_IRQHandler, DMA1_Channel5_IRQn, 2);
    DMA1_Channel5->CCR = 0x2922;
    DMA1_Channel5->CCR = 0x2923;
    TIM1->EGR = TIM_EGR_UG;
    TIM1->CR1 = TIM_CR1_CEN;

    gpio_peripheral(GPIO('B', 10), GPIO_FUNCTION(0), 0);
    gpio_peripheral(GPIO('B', 11), GPIO_INPUT, 0);
    enable_pclock(USART3_BASE);
    USART3->BRR = 156;
    USART3->CR2 = 0;
    USART3->CR3 = 0x00c0;
    USART3->CR1 = 0x201c;

    DMA1_Channel2->CCR = 0;
    DMA1_Channel2->CPAR = (uint32_t)(uintptr_t)&USART3->DR;
    DMA1_Channel2->CMAR = (uint32_t)(uintptr_t)tmc_request;
    DMA1_Channel2->CNDTR = sizeof(tmc_request);
    *n32_dma_channel_selector(DMA1_Channel2) = 5;
    DMA1_Channel2->CCR = 0x3092;

    DMA1_Channel3->CCR = 0;
    DMA1_Channel3->CPAR = (uint32_t)(uintptr_t)&USART3->DR;
    DMA1_Channel3->CMAR = (uint32_t)(uintptr_t)tmc_response;
    DMA1_Channel3->CNDTR = sizeof(tmc_response);
    *n32_dma_channel_selector(DMA1_Channel3) = 11;
    DMA1_Channel3->CCR = 0x2082;

    enable_pclock(TIM4_BASE);
    TIM4->PSC = 719;
    TIM4->ARR = 99;
    TIM4->CNT = 0;
    TIM4->EGR = TIM_EGR_UG;
    TIM4->DIER = TIM_DIER_UIE;
    TIM4->CR1 = 0;

    armcm_enable_irq(DMA1_Channel2_IRQHandler, DMA1_Channel2_IRQn, 14);
    armcm_enable_irq(DMA1_Channel3_IRQHandler, DMA1_Channel3_IRQn, 14);
    armcm_enable_irq(TIM4_IRQHandler, TIM4_IRQn, 0);
    armcm_enable_irq(USART3_IRQHandler, USART3_IRQn, 14);
    DMA1_Channel3->CCR |= 1u;

    // TMC UART write: sync, slave, register | 0x80, big-endian data, CRC.
    static const uint8_t tmc_init[][8] = {
        // GCONF (0x00) = 0x000001d0
        { 0x05, 0x00, 0x80, 0x00, 0x00, 0x01, 0xd0, 0xce },
        // IHOLD_IRUN (0x10) = 0x000a0c0c
        { 0x05, 0x00, 0x90, 0x00, 0x0a, 0x0c, 0x0c, 0x1d },
        // TPOWERDOWN (0x11) = 0x00000080
        { 0x05, 0x00, 0x91, 0x00, 0x00, 0x00, 0x80, 0xc0 },
        // CHOPCONF (0x6c) = 0x140082c3
        { 0x05, 0x00, 0xec, 0x14, 0x00, 0x82, 0xc3, 0x23 },
        // PWMCONF (0x70) = 0xc80d174b
        { 0x05, 0x00, 0xf0, 0xc8, 0x0d, 0x17, 0x4b, 0x77 },
    };
    for (uint_fast8_t i = 0; i < sizeof(tmc_init) / sizeof(tmc_init[0]); i++)
        c5_tmc_boot_write(tmc_init[i], sizeof(tmc_init[i]));

    c5_eboard_init();
}
DECL_INIT(c5_eboard_hardware_init);
