#include "printf.h"
#include "utils.h"
#include "timer.h"
#include "irq.h"
#include "sched.h"
#include "fork.h"
#include "mini_uart.h"
#include "sys.h"

/* reasonable rate of printing characters...*/
#ifdef USE_QEMU
#define DELAYS 5000000
#else
#define DELAYS 500000
#endif

#define RUN_SYSREG_TEST 0

struct priority_task_arg {
	char *label;
	long initial_priority;
	long later_priority;
	int switch_after_rounds;
};

static struct priority_task_arg priority_task_a = {"A", 1, 1, 0};
static struct priority_task_arg priority_task_b = {"B", 1, 5, 12};

void user_process1(char *array)
{
	char buf[2] = {0};
	while (1){
		for (int i = 0; i < 5; i++){
			buf[0] = array[i];
			call_sys_write(buf);
			delay(DELAYS);
		}
	}
}

void user_sysreg_test(void)
{
	char buf[60];
	tfp_sprintf(buf, "Try reading sctlr_el1 from EL0\n\r");
	call_sys_write(buf);
	asm volatile("mrs x0, sctlr_el1");
	tfp_sprintf(buf, "Should not reach here\n\r");
	call_sys_write(buf);
	call_sys_exit();
}

void user_priority_process(struct priority_task_arg *arg)
{
	char buf[40];
	char out[2] = {0};
	int rounds = 0;
	int changed = 0;

	call_sys_set_priority(arg->initial_priority);
	tfp_sprintf(buf, "\n\r%s priority=%d\n\r", arg->label, (int)arg->initial_priority);
	call_sys_write(buf);

	while (1) {
		out[0] = arg->label[0];
		call_sys_write(out);
		delay(DELAYS);
		rounds++;
		if (!changed && arg->switch_after_rounds && rounds >= arg->switch_after_rounds) {
			call_sys_set_priority(arg->later_priority);
			tfp_sprintf(buf, "\n\r%s priority=%d\n\r", arg->label, (int)arg->later_priority);
			call_sys_write(buf);
			changed = 1;
		}
	}
}

void user_process(){
	char buf[80];
	tfp_sprintf(buf, "User process started\n\r");
	call_sys_write(buf);
	tfp_sprintf(buf, "Priority demo: B raises priority from 1 to 5 while running\n\r");
	call_sys_write(buf);
	unsigned long stack = call_sys_malloc();
	if (stack < 0) {
		printf("Error while allocating stack for process 1\n\r");
		return;
	}
	int err = call_sys_clone((unsigned long)&user_priority_process, (unsigned long)&priority_task_a, stack);
	if (err < 0){
		printf("Error while clonning process 1\n\r");
		return;
	} 
	stack = call_sys_malloc();
	if (stack < 0) {
		printf("Error while allocating stack for process 1\n\r");
		return;
	}
	err = call_sys_clone((unsigned long)&user_priority_process, (unsigned long)&priority_task_b, stack);
	if (err < 0){
		printf("Error while clonning process 2\n\r");
		return;
	} 
	call_sys_exit();
}

void kernel_process(){
	printf("Kernel process started. EL %d\r\n", get_el());
#if RUN_SYSREG_TEST
	int err = move_to_user_mode((unsigned long)&user_sysreg_test);
#else
	int err = move_to_user_mode((unsigned long)&user_process);
#endif
	if (err < 0) {
		printf("Error while moving process to user mode\n\r");
	} 
	// this func is called from ret_from_fork (entry.S). after returning, it goes back to 
	// ret_from_fork and does kernel_exit there. hence, pt_regs populated by move_to_user_mode()
	// will take effect. 
}

void kernel_main(void)
{
	uart_init();
	init_printf(0, putc);

	printf("kernel boots...\n\r");

	irq_vector_init();
	generic_timer_init();
	enable_interrupt_controller();
	enable_irq();

	int res = copy_process(PF_KTHREAD, (unsigned long)&kernel_process, 0, 0);
	if (res < 0) {
		printf("error while starting kernel process");
		return;
	}

	while (1) {
		schedule();
	}	
}
