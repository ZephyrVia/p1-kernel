#ifndef	_P_UART_H
#define	_P_UART_H

#include "peripherals/base.h"

#define UART0_BASE  (PBASE + 0x00201000)

#define UART0_DR    (UART0_BASE + 0x00)
#define UART0_FR    (UART0_BASE + 0x18)
#define UART0_IBRD  (UART0_BASE + 0x24)
#define UART0_FBRD  (UART0_BASE + 0x28)
#define UART0_LCRH  (UART0_BASE + 0x2C)
#define UART0_CR    (UART0_BASE + 0x30)
#define UART0_IMSC  (UART0_BASE + 0x38)
#define UART0_ICR   (UART0_BASE + 0x44)

#define UART_FR_RXFE  (1 << 4)
#define UART_FR_TXFF  (1 << 5)

#define UART_LCRH_FEN       (1 << 4)
#define UART_LCRH_WLEN_8BIT (3 << 5)

#define UART_CR_UARTEN (1 << 0)
#define UART_CR_TXE    (1 << 8)
#define UART_CR_RXE    (1 << 9)

#endif  /*_P_UART_H */
