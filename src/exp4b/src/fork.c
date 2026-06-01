#include "mm.h"
#include "sched.h"
#include "entry.h"
#include "debug.h"

int copy_process_with_priority(unsigned long fn, unsigned long arg, long priority)
{
	preempt_disable();
	struct task_struct *p;

	p = (struct task_struct *) get_free_page();
	if (!p) {
		preempt_enable();
		return 1;
	}
	memzero((unsigned long)p, THREAD_SIZE);
	if (priority < 1) {
		priority = 1;
	}
	p->priority = priority;
	p->state = TASK_RUNNING;
	p->counter = p->priority;
	p->preempt_count = 1; //disable preemtion until schedule_tail

	p->cpu_context.x19 = fn;
	p->cpu_context.x20 = arg;
	p->cpu_context.pc = (unsigned long)ret_from_fork;	// entry.S
	p->cpu_context.sp = (unsigned long)p + THREAD_SIZE;
	int pid = nr_tasks++;
	task[pid] = p;	
	debug_task_created(pid, p, fn, arg);
	preempt_enable();
	return 0;
}

int copy_process(unsigned long fn, unsigned long arg)
{
	return copy_process_with_priority(fn, arg, current->priority);
}
