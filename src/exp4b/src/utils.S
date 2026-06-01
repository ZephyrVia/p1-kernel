.globl get_el
get_el:
	mrs x0, CurrentEL
	lsr x0, x0, #2
	ret

.globl get_sp
get_sp:
	mov x0, sp
	ret

.globl set_simd_marker
set_simd_marker:
	dup v0.2d, x0
	ret

.globl get_simd_marker
get_simd_marker:
	fmov x0, d0
	ret

.globl put32
put32:
	str w1,[x0]
	ret

.globl get32
get32:
	ldr w0,[x0]
	ret

.globl delay
delay:
	subs x0, x0, #1
	bne delay
	ret
