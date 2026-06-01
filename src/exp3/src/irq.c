#include "utils.h"
#include "printf.h"
#include "timer.h"
#include "mini_uart.h"
#include "entry.h"
#include "peripherals/irq.h"

const char *entry_error_messages[] = {
    "SYNC_INVALID_EL1t",
    "IRQ_INVALID_EL1t",		
    "FIQ_INVALID_EL1t",		
    "ERROR_INVALID_EL1T",		

    "SYNC_INVALID_EL1h",		
    "IRQ_INVALID_EL1h",		
    "FIQ_INVALID_EL1h",		
    "ERROR_INVALID_EL1h",		

    "SYNC_INVALID_EL0_64",		
    "IRQ_INVALID_EL0_64",		
    "FIQ_INVALID_EL0_64",		
    "ERROR_INVALID_EL0_64",	

    "SYNC_INVALID_EL0_32",		
    "IRQ_INVALID_EL0_32",		
    "FIQ_INVALID_EL0_32",		
    "ERROR_INVALID_EL0_32"	
};

void enable_interrupt_controller()
{
    put32(TIMER_INT_CTRL_0, TIMER_INT_CTRL_0_VALUE);
    put32(ENABLE_IRQS_1, IRQ_PENDING_1_AUX);
}

void show_invalid_entry_message(int type, unsigned long esr, unsigned long address)
{
    printf("%s, ESR: %x, address: %x\r\n", entry_error_messages[type], esr, address);
}

void handle_irq(void)
{
    unsigned int handled = 0;
    unsigned int local_irq = get32(INT_SOURCE_0);
    unsigned int irq_pending_1 = get32(IRQ_PENDING_1);

    if (local_irq & GENERIC_TIMER_INTERRUPT) {
        handle_local_timer_irq();
        handled = 1;
    }

    if (irq_pending_1 & IRQ_PENDING_1_AUX) {
        handle_uart_irq();
        handled = 1;
    }

    if (!handled) {
        printf("Unknown pending irq: local=%x irq1=%x\r\n", local_irq, irq_pending_1);
    }
}
