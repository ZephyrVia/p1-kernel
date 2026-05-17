#include "mm.h"

# specify that everything defined in boot.S should go in the .text.boot section. 
.section ".text.boot"

# this section is placed at the beginning of the kernel image by the linker script
# when the kernel is started, execution begins at the `start` function
.globl _start
_start:

	# Rpi3 has 4 cores, and after the device is powered on, each core begins to 
	# execute the same code. Keep each core's processor id and give it a separate
	# stack before entering C code.

	# Get the processor ID from the [mpidr_el1](http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.ddi0500g/BABHBJCI.html) 
	# system register. 	
	mrs	x0, mpidr_el1		
	and	x0, x0, #0xFF		// Check processor id
	mov	x19, x0				// Preserve processor id across setup.
	add	x1, x19, #1
	mov	x2, #CORE_STACK_SIZE
	mul	x1, x1, x2
	mov	x2, #LOW_MEMORY
	add	x1, x1, x2
	mov	sp, x1
	cbz	x19, master
	b	wait_for_master

proc_hang: 
	b 	proc_hang

# If the current processor ID is 0, then execution branches to the `master` function
master:

	# Here, we clean the `.bss` section by calling `memzero`. 
	# In ARMv8 architecture, by convention, the first seven arguments are passed to 
	# the called function via registers x0–x6. 
	# The `memzero` function accepts only two arguments: the start address (`bss_begin`) 
	# and the size of the section needed to be cleaned (`bss_end - bss_begin`).
	#	bss_begin/end are defined in the linker script
	adr	x0, bss_begin
	adr	x1, bss_end
	sub	x1, x1, x0
	bl 	memzero

	# Release secondary cores. On Rpi3/QEMU raspi3b, core1-3 wait in the
	# firmware/QEMU holding pen. Each core watches a fixed low-memory spin-table
	# slot for its entry address:
	#   core1: 0xe0
	#   core2: 0xe8
	#   core3: 0xf0
	# Write our _start address to those slots, then send an event. The waiting
	# cores wake up, read their slot, and branch to _start.
	ldr	x0, =_start
	mov	x1, #0xe0
	str	x0, [x1]
	mov	x1, #0xe8
	str	x0, [x1]
	mov	x1, #0xf0
	str	x0, [x1]
	sev

	mov	x0, #1
	adr	x1, boot_ready
	stlr	w0, [x1]

	b	kernel_entry

wait_for_master:
	adr	x1, boot_ready
1:
	ldar	w0, [x1]
	cbz	w0, 1b

kernel_entry:
	mov	x0, x19
	bl	kernel_main		// c function, kernel.c
	b 	proc_hang		// should never reach here

.section ".data"
.balign 4
boot_ready:
	.word 0
