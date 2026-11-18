#ifndef PW_NATIVE_MACHINE_H
#define PW_NATIVE_MACHINE_H

/* Guest tasks run serially; the host owns interrupt masking and sleep. */
static inline void set_ccr(unsigned char value) { (void)value; }
/* Deliberately undefined: unused H8 routines must be discarded at link time. */
void sleep(void);
void nop(void);

#endif
