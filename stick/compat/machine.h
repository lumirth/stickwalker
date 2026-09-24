#ifndef PW_STICK_MACHINE_H
#define PW_STICK_MACHINE_H

/* These intrinsics are H8 CPU operations. A Stick implementation must handle
 * sleep, interrupt masking, and watchdog policy in its own runtime. Retaining
 * the calls as no-ops only lets individual application modules be compiled
 * during the migration; this header does not make them runnable. */
#define nop() ((void)0)
#define sleep() ((void)0)
#define set_ccr(value) ((void)(value))
#define get_ccr() 0

#endif
