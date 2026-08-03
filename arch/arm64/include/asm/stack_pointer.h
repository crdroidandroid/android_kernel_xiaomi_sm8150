/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_STACK_POINTER_H
#define __ASM_STACK_POINTER_H

/*
 * How to get the current stack pointer from C.
 */
static __always_inline unsigned long __current_stack_pointer(void)
{
	unsigned long sp;

	asm volatile("mov %0, sp" : "=r" (sp));
	return sp;
}

#define current_stack_pointer	__current_stack_pointer()

#endif /* __ASM_STACK_POINTER_H */
