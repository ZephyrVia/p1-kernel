#include "sync.h"
#include "uart.h"

static unsigned int uart_ready;
static unsigned int uart_lock;

void kernel_main(unsigned int processor_id)
{
	if (processor_id == 0) {
		uart_init();
		store_release(&uart_ready, 1);
	}

	while (!load_acquire(&uart_ready)) {
	}

	spin_lock(&uart_lock);
	uart_send_string("Hello, from processor ");
	uart_send('0' + processor_id);
	uart_send_string("\r\n");
	spin_unlock(&uart_lock);

	while (1) {
	}
}
