#ifndef PW_STICK_MACHINE_H
#define PW_STICK_MACHINE_H

#include "stick/native_runtime.h"

#define nop() ((void)0)
#define sleep() StickNativeService()
#define set_ccr(value) StickNativeSetCcr((uint8_t)(value))
#define get_ccr() StickNativeGetCcr()

#endif
