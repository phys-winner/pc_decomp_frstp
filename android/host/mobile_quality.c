#include "cpu64.h"
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#endif

int frostpunk_dxvk_mobile_effects(void);
int frostpunk_mobile_graphics_reduced(unsigned group); /* port_settings.h: FP_GFX_EFFECTS = 4 */
void fp_quality_original_L_000140F752A0(CPU *);
void fp_quality_original_L_0001410343F0(CPU *);

static unsigned quality_mode(void) {
#ifdef __ANDROID__
    static _Atomic int override = -2;
    int mode = atomic_load_explicit(&override, memory_order_relaxed);
    if (mode == -2) {
        char value[PROP_VALUE_MAX] = {0}; int saved = errno;
        __system_property_get("debug.fp.quality", value);
        mode = value[0] ? (int)strtoul(value, NULL, 0) & 3 : -1;
        atomic_store_explicit(&override, mode, memory_order_relaxed);
        errno = saved;
    }
    if (mode >= 0) return (unsigned)mode;
#endif
    return frostpunk_mobile_graphics_reduced(4u) ? 3u : 0u;
}

int fp_mobile_particle_fill_disabled(void) {
#ifdef __ANDROID__
    static _Atomic int override = -2;
    int mode = atomic_load_explicit(&override, memory_order_relaxed);
    if (mode == -2) {
        char value[PROP_VALUE_MAX] = {0}; int saved = errno;
        __system_property_get("debug.fp.extra_fx", value);
        mode = value[0] ? !!(strtoul(value, NULL, 0) & 4) : -1;
        atomic_store_explicit(&override, mode, memory_order_relaxed);
        errno = saved;
    }
    if (mode >= 0) return mode;
#endif
    return quality_mode() == 3u;
}

void L_000140F752A0(CPU *c) {
    unsigned mode = quality_mode();
    fp_quality_original_L_000140F752A0(c);
    if (mode & 1) SET8L(c->rax, 0);
}

void L_0001410343F0(CPU *c) {
    unsigned mode = quality_mode();
    fp_quality_original_L_0001410343F0(c);
    if (mode & 2) SET8L(c->rax, 0);
}
