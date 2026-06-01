#include "printf.h"
#include "utils.h"
#include "timer.h"
#include "irq.h"
#include "fork.h"
#include "sched.h"
#include "mini_uart.h"
#include "debug.h"
#include "mm.h"

#define PROCESS1_PRIORITY 1
#define PROCESS2_PRIORITY 3
#define PROCESS1_SIMD_MARKER 0x11111111UL
#define PROCESS2_SIMD_MARKER 0x22222222UL

void process(char *array)
{
	set_simd_marker(PROCESS1_SIMD_MARKER);
	printf("[fpsimd] process 1 marker=%x\r\n", PROCESS1_SIMD_MARKER);
	while (1) {
		for (int i = 0; i < 5; i++){
			if (get_simd_marker() != PROCESS1_SIMD_MARKER) {
				printf("[fpsimd] process 1 marker corrupted: %x\r\n", get_simd_marker());
				set_simd_marker(PROCESS1_SIMD_MARKER);
			}
			uart_send(array[i]);
			delay(5000000);
		}
	}
}

void process2(char *array)
{
	set_simd_marker(PROCESS2_SIMD_MARKER);
	printf("[fpsimd] process 2 marker=%x\r\n", PROCESS2_SIMD_MARKER);
	while (1) {
		for (int i = 0; i < 5; i++){
			if (get_simd_marker() != PROCESS2_SIMD_MARKER) {
				printf("[fpsimd] process 2 marker corrupted: %x\r\n", get_simd_marker());
				set_simd_marker(PROCESS2_SIMD_MARKER);
			}
			uart_send(array[i]);
			delay(5000000);
		}
	}
}

void kernel_main(void)
{
	uart_init();
	init_printf(0, putc);

	printf("kernel boots\n");
	debug_boot("entered kernel_main");

	mm_init();
	printf("[mm] alloc_start=%x free_pages=%u\r\n", get_alloc_start(), get_free_pages_count());

	irq_vector_init();
	debug_boot("irq vectors installed");
	generic_timer_init();
	debug_boot("generic timer initialized");
	enable_interrupt_controller();
	debug_boot("interrupt controller enabled");
	enable_irq();
	debug_boot("cpu irq enabled");

	int res = copy_process_with_priority((unsigned long)&process,
					     (unsigned long)"12345",
					     PROCESS1_PRIORITY);
	if (res != 0) {
		printf("error while starting process 1");
		return;
	}
	debug_boot("process 1 created");
	res = copy_process_with_priority((unsigned long)&process2,
					 (unsigned long)"abcde",
					 PROCESS2_PRIORITY);
	if (res != 0) {
		printf("error while starting process 2");
		return;
	}
	debug_boot("process 2 created");

	while (1){
		schedule();
	}	
}
