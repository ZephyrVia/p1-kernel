#include "utils.h"
#include "printf.h"
#include "timer.h"

#ifdef USE_QEMU
static int interval = (1 << 26); // around 1 sec in QEMU
#else
static int interval = 1 * 1000 * 1000; // around 1 sec on RPi3
#endif

static unsigned int read_cntfrq(void)
{
	unsigned int val;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (val));
	return val;
}

void local_timer_init(void)
{
	unsigned int freq = read_cntfrq();
	printf("System count freq (CNTFRQ) is: %u\n", freq);

	printf("interval is set to: %d\n", interval);
	local_timer_enable();
	local_timer_reset(interval);
}

void handle_local_timer_irq(void)
{
	printf("Timer interrupt received. next in %d ticks\n\r", interval);
	local_timer_reset(interval);
}
