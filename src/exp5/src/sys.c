#include "fork.h"
#include "printf.h"
#include "utils.h"
#include "sched.h"
#include "mm.h"

void sys_write(char * buf){
	printf(buf);
}

int sys_set_priority(long priority)
{
	if (priority < 1) {
		return -1;
	}
	preempt_disable();
	printf("[prio] task=%x priority %d -> %d\r\n", current, current->priority, priority);
	current->priority = priority;
	current->counter = priority;
	preempt_enable();
	return 0;
}

/* Creates a new user thread. 
	@stack: The location of the stack for the newly created thread. */
int sys_clone(unsigned long stack){
	/* Create a blank task and we're done. The new (child) task's fn and arg will be set right after 
	returning from the clone() syscall, to the calling task at the user level, cf: thread_start() in sys.S */
	return copy_process(0 /*clone_flags*/, 0 /*fn*/, 0 /*arg*/, stack);
}

/* Allocates a memory page for a user process */
unsigned long sys_malloc(){
	unsigned long addr = get_free_page();
	if (!addr) {
		return -1;
	}
	return addr;
}

/* Each process must call this syscall after it finishes execution. */
void sys_exit(){
	exit_process();
}

/* An array of pointers to all syscall handlers. 
	Each syscall has a "syscall number" (sys.h) — which is just an index in this array */
void * const sys_call_table[] = {sys_write, sys_malloc, sys_clone, sys_exit, sys_set_priority};
