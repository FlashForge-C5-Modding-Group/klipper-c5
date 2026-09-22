// USART0 support for GD32H737
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_SERIAL_BAUD
#include "generic/armcm_boot.h" // armcm_enable_irq
#include "generic/serial_irq.h" // serial_rx_byte
#include "command.h" // DECL_CONSTANT_STR
#include "compiler.h" // DIV_ROUND_CLOSEST
#include "internal.h" // USART0
#include "sched.h" // DECL_INIT

DECL_CONSTANT_STR("RESERVE_PINS_serial", "PA10,PA9");

void
USART0_IRQHandler(void)
{
    uint32_t stat = USART0->STAT;
    if (stat & USART_STAT_RBNE)
        serial_rx_byte(USART0->RDATA);
    if ((stat & USART_STAT_TBE) && (USART0->CTL0 & USART_CTL0_TBEIE)) {
        uint8_t data;
        if (serial_get_tx_byte(&data))
            USART0->CTL0 &= ~USART_CTL0_TBEIE;
        else
            USART0->TDATA = data;
    }
    USART0->INTC = stat & (USART_STAT_PERR | USART_STAT_FERR
                           | USART_STAT_NERR | USART_STAT_ORERR);
}
DECL_ARMCM_IRQ(USART0_IRQHandler, USART0_IRQn);

void
serial_enable_tx_irq(void)
{
    USART0->CTL0 |= USART_CTL0_TBEIE;
}

void
serial_init(void)
{
    RCU_CFG1 = (RCU_CFG1 & ~RCU_CFG1_USART0SEL) | RCU_USARTSRC_APB;
    enable_pclock(USART0_BASE);
    gpio_peripheral(GPIO('A', 10), GPIO_FUNCTION(7), 1);
    gpio_peripheral(GPIO('A', 9), GPIO_FUNCTION(7), 0);

    USART0->CTL0 = 0;
    USART0->CTL1 = 0;
    USART0->CTL2 = 0;
    uint32_t pclk = get_pclock_frequency(USART0_BASE);
    USART0->BAUD = DIV_ROUND_CLOSEST(pclk, CONFIG_SERIAL_BAUD);
    USART0->INTC = USART_INTC_PEC | USART_INTC_FEC
                   | USART_INTC_NEC | USART_INTC_OREC;
    armcm_enable_irq(USART0_IRQHandler, USART0_IRQn, 0);
    USART0->CTL0 = USART_CTL0_UEN | USART_CTL0_REN | USART_CTL0_TEN
                   | USART_CTL0_RBNEIE;
}
DECL_INIT(serial_init);
