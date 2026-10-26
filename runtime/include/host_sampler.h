#ifndef PSX_HOST_SAMPLER_H
#define PSX_HOST_SAMPLER_H
/* Always-on host CPU sampler for the emulation thread (debug-tools builds).
 * See host_sampler.c; TCP host_profile; tools/host_profile.py. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_SAMPLER_CAP 65536u   /* ~65 s at 1 kHz */

typedef struct HostSample {
    uint64_t rva;      /* instruction pointer minus the executable's base */
    uint64_t caller;   /* outside the executable (a system DLL, the driver): the
                        * first return address back inside it, relative to
                        * its base (Windows x64; 0 = none or unknown) */
    uint64_t caller2;  /* the next one up (who called the caller) */
    uint32_t frame;    /* guest frame (s_frame_count) */
    uint16_t in_pass;  /* 1 while a render pass ran */
    uint16_t outside;  /* 1: the instruction pointer was outside the executable */
} HostSample;

/* Start sampling the calling thread (the emulation thread). Idempotent. */
void host_sampler_start(void);
/* Called by the render thread when it starts: with PSX_HOST_SAMPLER_THREAD=render
 * (Windows) the sampler samples that thread instead, into the same ring. */
void host_sampler_register_render_thread(void);
int host_sampler_supported(void);
/* Total samples recorded; the ring holds the newest HOST_SAMPLER_CAP. */
uint64_t host_sampler_seq(void);
uint64_t host_sampler_image_base(void);
/* Sample `seq`, if it is still in the ring. */
int host_sampler_get(uint64_t seq, HostSample *out);

#ifdef __cplusplus
}
#endif
#endif
