#ifndef	_P_MINI_UART_H
#define	_P_MINI_UART_H

#include "peripherals/base.h"

#define AUX_IRQ_REG     (PBASE+0x00215000)
#define AUX_ENABLES     (PBASE+0x00215004)
#define AUX_MU_IO_REG   (PBASE+0x00215040)
#define AUX_MU_IER_REG  (PBASE+0x00215044)
#define AUX_MU_IIR_REG  (PBASE+0x00215048)
#define AUX_MU_LCR_REG  (PBASE+0x0021504C)
#define AUX_MU_MCR_REG  (PBASE+0x00215050)
#define AUX_MU_LSR_REG  (PBASE+0x00215054)
#define AUX_MU_MSR_REG  (PBASE+0x00215058)
#define AUX_MU_SCRATCH  (PBASE+0x0021505C)
#define AUX_MU_CNTL_REG (PBASE+0x00215060)
#define AUX_MU_STAT_REG (PBASE+0x00215064)
#define AUX_MU_BAUD_REG (PBASE+0x00215068)

#define AUX_IRQ_MINI_UART      (1 << 0)

#define AUX_MU_IER_RX_IRQ      (1 << 0)
#define AUX_MU_IER_TX_IRQ      (1 << 1)

#define AUX_MU_IIR_IRQ_PENDING (1 << 0)
#define AUX_MU_IIR_RX_IRQ      (2 << 1)
#define AUX_MU_IIR_TX_IRQ      (1 << 1)

#define AUX_MU_LSR_DATA_READY  (1 << 0)
#define AUX_MU_LSR_TX_EMPTY    (1 << 5)

#endif  /*_P_MINI_UART_H */
