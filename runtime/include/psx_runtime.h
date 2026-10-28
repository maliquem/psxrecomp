#ifndef PSXRECOMP_PSX_RUNTIME_H
#define PSXRECOMP_PSX_RUNTIME_H

#include "cpu_state.h"
#include "debug_server.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_edge.h"   /* last: makes psx_check_interrupts_at the inline edge */

#ifdef __cplusplus
extern "C" {
#endif

/* Fix B exception-return escape state + helpers are declared in cpu_state.h
 * (the lowest common header the generated BIOS/game both include). */

static inline void call_by_address(CPUState* cpu, uint32_t addr) {
    psx_dispatch_call(cpu, addr, cpu->gpr[31]);
}

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_RUNTIME_H */
