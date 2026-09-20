#ifndef __GD32H7_H
#define __GD32H7_H

#include <stdint.h>

// Cortex-M7 device facts consumed by the repository CMSIS core.
#define __CM7_REV                 0x0102U
#define __MPU_PRESENT             1U
#define __NVIC_PRIO_BITS          4U
#define __Vendor_SysTickConfig    0U
#define __FPU_PRESENT             1U
#define __ICACHE_PRESENT          1U
#define __DCACHE_PRESENT          1U

typedef enum {
    SysTick_IRQn = -1,
    USART0_IRQn = 37,
} IRQn_Type;

#include "core_cm7.h"

typedef struct {
    __IO uint32_t MODER;          // 0x00
    __IO uint32_t OTYPER;         // 0x04
    __IO uint32_t OSPEEDR;        // 0x08
    __IO uint32_t PUPDR;          // 0x0c
    __I  uint32_t IDR;            // 0x10
    __IO uint32_t ODR;            // 0x14
    __O  uint32_t BOP;            // 0x18, atomic set
    __IO uint32_t LCKR;           // 0x1c
    __IO uint32_t AFR[2];         // 0x20, 0x24
    __O  uint32_t BC;             // 0x28, atomic clear
    __O  uint32_t TG;             // 0x2c, atomic toggle
} GPIO_TypeDef;

typedef struct {
    __IO uint32_t CTL0;           // 0x00
    __IO uint32_t CTL1;           // 0x04
    __IO uint32_t CTL2;           // 0x08
    __IO uint32_t BAUD;           // 0x0c
    __IO uint32_t GP;             // 0x10
    __IO uint32_t RT;             // 0x14
    __O  uint32_t CMD;            // 0x18
    __I  uint32_t STAT;           // 0x1c
    __O  uint32_t INTC;           // 0x20
    __I  uint32_t RDATA;          // 0x24
    __O  uint32_t TDATA;          // 0x28
} USART_TypeDef;

#define RCU_BASE                0x58024400UL
#define SYSCFG_BASE             0x58000400UL
#define FWDGT_BASE              0x58004800UL
#define GPIOA_BASE              0x58020000UL
#define GPIOB_BASE              0x58020400UL
#define GPIOC_BASE              0x58020800UL
#define GPIOD_BASE              0x58020C00UL
#define GPIOE_BASE              0x58021000UL
#define USART0_BASE             0x40011000UL
#define ADC0_BASE               0x40012400UL
#define ADC1_BASE               0x40012800UL
#define ADC2_BASE               0x40012C00UL
#define TIMER1_BASE             0x40000000UL
#define TIMER2_BASE             0x40000400UL
#define TIMER3_BASE             0x40000800UL
#define TIMER4_BASE             0x40000C00UL
#define TIMER7_BASE             0x40010400UL
#define TIMER22_BASE            0x4000E000UL

#define GPIOA                  ((GPIO_TypeDef *)GPIOA_BASE)
#define GPIOB                  ((GPIO_TypeDef *)GPIOB_BASE)
#define GPIOC                  ((GPIO_TypeDef *)GPIOC_BASE)
#define GPIOD                  ((GPIO_TypeDef *)GPIOD_BASE)
#define GPIOE                  ((GPIO_TypeDef *)GPIOE_BASE)
#define USART0                 ((USART_TypeDef *)USART0_BASE)

#endif // gd32h7.h
