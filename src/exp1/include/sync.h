#ifndef	_SYNC_H
#define	_SYNC_H

void store_release(unsigned int* addr, unsigned int value);
unsigned int load_acquire(unsigned int* addr);
void spin_lock(unsigned int* lock);
void spin_unlock(unsigned int* lock);

#endif  /*_SYNC_H */
