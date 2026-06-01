#ifndef	_TIMER_H
#define	_TIMER_H

void local_timer_init(void);
void handle_local_timer_irq(void);

extern void local_timer_enable(void);
extern void local_timer_reset(int interval);

#endif  /*_TIMER_H */
