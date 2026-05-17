.globl store_release
store_release:
	stlr	w1, [x0]
	ret

.globl load_acquire
load_acquire:
	ldar	w0, [x0]
	ret

.globl spin_lock
spin_lock:
	mov	w2, #1
1:
	ldaxr	w1, [x0]
	cbnz	w1, 2f
	stxr	w1, w2, [x0]
	cbnz	w1, 1b
	ret
2:
	clrex
	b	1b

.globl spin_unlock
spin_unlock:
	stlr	wzr, [x0]
	ret
