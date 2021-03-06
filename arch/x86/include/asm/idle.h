#ifndef _ASM_X86_IDLE_H
#define _ASM_X86_IDLE_H

#ifdef CONFIG_X86_64
void enter_idle(void);
void exit_idle(void);
#else 
static inline void enter_idle(void) { }
static inline void exit_idle(void) { }
static inline void __exit_idle(void) { }
#endif 

void amd_e400_remove_cpu(int cpu);

#endif 
