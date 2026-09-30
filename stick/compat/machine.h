#ifndef PW_STICK_MACHINE_H
#define PW_STICK_MACHINE_H

/* H8 CPU intrinsics are no-ops here. board_runtime.cpp and power_sleep.cpp
 * own scheduling, processor sleep and interrupt policy on the Stick. */
#define nop() ((void)0)
#define sleep() ((void)0)
#define set_ccr(value) ((void)(value))
#define get_ccr() 0

#endif
