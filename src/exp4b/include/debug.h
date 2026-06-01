#ifndef _DEBUG_H
#define _DEBUG_H

#include "sched.h"

#define DBG_BOOT_LIMIT      16
#define DBG_ALLOC_LIMIT     16
#define DBG_SCHED_LIMIT     48
#define DBG_SWITCH_LIMIT    48
#define DBG_TIMER_LIMIT     32
#define DBG_IRQ_LIMIT       16

void debug_boot(const char *event);
void debug_task_created(int pid, struct task_struct *p, unsigned long fn, unsigned long arg);
void debug_schedule_pick(const char *reason, struct task_struct *cur, int next, int counter);
void debug_switch(struct task_struct *prev, struct task_struct *next);
void debug_timer_tick(struct task_struct *cur, long before, long after, int will_schedule);
void debug_irq(unsigned int irq);
void debug_task_start(void);

#endif
