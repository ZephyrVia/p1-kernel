#include "utils.h"
#include "peripherals/uart.h"
#include "peripherals/gpio.h"

static const unsigned int uart_clock_freq = 48000000;
static const unsigned int baud_rate = 115200;

static void uart_baud_reg_values(unsigned int baud, unsigned int* ibrd, unsigned int* fbrd)
{
	unsigned int baud_divisor = 16 * baud;
	unsigned int remainder = uart_clock_freq % baud_divisor;

	*ibrd = uart_clock_freq / baud_divisor;
	*fbrd = ((remainder * 64) + (baud_divisor / 2)) / baud_divisor;

	if (*fbrd == 64) {
		*ibrd += 1;
		*fbrd = 0;
	}
}

void uart_send ( char c )
{
	while(1) {
		if(!(get32(UART0_FR)&UART_FR_TXFF))
			break;
	}
	put32(UART0_DR,c);
}

char uart_recv ( void )
{
	while(1) {
		if(!(get32(UART0_FR)&UART_FR_RXFE))
			break;
	}
	return(get32(UART0_DR)&0xFF);
}

void uart_send_string(char* str)
{
	for (int i = 0; str[i] != '\0'; i ++) {
		uart_send((char)str[i]);
	}
}

void uart_init ( void )
{
	unsigned int selector;
	unsigned int ibrd;
	unsigned int fbrd;

	put32(UART0_CR,0);                     // Disable UART0 before configuring it

	selector = get32(GPFSEL1);
	selector &= ~(7<<12);                   // clean gpio14
	selector |= 4<<12;                      // set alt0 for gpio14
	selector &= ~(7<<15);                   // clean gpio15
	selector |= 4<<15;                      // set alt0 for gpio15
	put32(GPFSEL1,selector);

	put32(GPPUD,0);
	delay(150);
	put32(GPPUDCLK0,(1<<14)|(1<<15));
	delay(150);
	put32(GPPUDCLK0,0);

	put32(UART0_ICR,0x7FF);                 // Clear pending interrupts
	put32(UART0_IMSC,0);                    // Disable UART interrupts

	uart_baud_reg_values(baud_rate,&ibrd,&fbrd);
	put32(UART0_IBRD,ibrd);
	put32(UART0_FBRD,fbrd);
	put32(UART0_LCRH,UART_LCRH_WLEN_8BIT | UART_LCRH_FEN);
	put32(UART0_CR,UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
}
