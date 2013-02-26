#ifndef __ASM_PARASITE_H__
#define __ASM_PARASITE_H__

static inline u32 arch_get_tls()
{
	return 0;
}

/*
 * Call thread daemon with new stack, the function must
 * return the original stack pointer passed in argument
 * argument 3
 */
#define call_daemon_thread(new_sp, args, s, func)	\
do {							\
	asm volatile ("movq %0, %%rax		\n"	\
		      "movq %1, %%rdi		\n"	\
		      "movq %2, %%rsi		\n"	\
		      "movq %%rsp, %%rdx	\n"	\
		      "movq %%rax, %%rsp	\n"	\
		      "call " #func "		\n"	\
		      "movq %%rax, %%rsp	\n"	\
		      :					\
		      : "g"(new_sp), "g"(args), "g"(s)	\
		      : "rax", "rdi", "rsi", "rdx",	\
		        "rsp", "memory");		\
} while (0)

#define asm_trap()					\
do {							\
	asm volatile("int3" ::);			\
	while (1) ;					\
} while (0)

#endif
