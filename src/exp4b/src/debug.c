#include "debug.h"
#include "printf.h"
#include "utils.h"

static int boot_logs;
static int alloc_logs;
static int sched_logs;
static int switch_logs;
static int timer_logs;
static int irq_logs;
static int start_logs;

static int allow(int *count, int limit)
{
	if (*count >= limit) {
		return 0;
	}
	(*count)++;
	return 1;
}

void debug_boot(const char *event)
{
	if (!allow(&boot_logs, DBG_BOOT_LIMIT)) {
		return;
	}
	printf("[boot] %s current=%x sp=%x nr_tasks=%u\r\n",
	       event, current, get_sp(), nr_tasks);
}

void debug_task_created(int pid, struct task_struct *p, unsigned long fn, unsigned long arg)
{
	if (!allow(&alloc_logs, DBG_ALLOC_LIMIT)) {
		return;
	}
	printf("[alloc] pid=%u task=%x fpsimd=%x stack_top=%x fn=%x arg=%x pc=%x priority=%u counter=%u preempt=%u\r\n",
	       pid, p, (unsigned long)&p->fpsimd_context,
	       (unsigned long)p + THREAD_SIZE, fn, arg, p->cpu_context.pc,
	       p->priority, p->counter, p->preempt_count);
}

void debug_schedule_pick(const char *reason, struct task_struct *cur, int next, int counter)
{
	if (!allow(&sched_logs, DBG_SCHED_LIMIT)) {
		return;
	}
	printf("[sched] %s cur=%x cur_prio=%u cur_counter=%u cur_preempt=%u next=%u next_task=%x next_prio=%u next_counter=%u sp=%x\r\n",
	       reason, cur, cur->priority, cur->counter, cur->preempt_count,
	       next, task[next], task[next]->priority, counter, get_sp());
}

void debug_switch(struct task_struct *prev, struct task_struct *next)
{
	if (!allow(&switch_logs, DBG_SWITCH_LIMIT)) {
		return;
	}
	printf("[switch] prev=%x next=%x prev_prio=%u next_prio=%u prev_sp=%x next_sp=%x prev_pc=%x next_pc=%x live_sp=%x\r\n",
	       prev, next, prev->priority, next->priority, prev->cpu_context.sp, next->cpu_context.sp,
	       prev->cpu_context.pc, next->cpu_context.pc, get_sp());
}

void debug_timer_tick(struct task_struct *cur, long before, long after, int will_schedule)
{
	if (!will_schedule && timer_logs >= 6) {
		return;
	}
	if (!allow(&timer_logs, DBG_TIMER_LIMIT)) {
		return;
	}
	printf("[timer] cur=%x priority=%u counter=%u->%u preempt=%u schedule=%u sp=%x\r\n",
	       cur, cur->priority, before, after, cur->preempt_count, will_schedule, get_sp());
}

void debug_irq(unsigned int irq)
{
	if (!allow(&irq_logs, DBG_IRQ_LIMIT)) {
		return;
	}
	printf("[irq] pending=%x current=%x sp=%x\r\n", irq, current, get_sp());
}

void debug_task_start(void)
{
	if (!allow(&start_logs, DBG_ALLOC_LIMIT)) {
		return;
	}
	printf("[task-start] current=%x priority=%u sp=%x pc=%x counter=%u preempt=%u\r\n",
	       current, current->priority, get_sp(), current->cpu_context.pc,
	       current->counter, current->preempt_count);
}
