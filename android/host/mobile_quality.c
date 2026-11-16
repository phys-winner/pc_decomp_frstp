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

static _Atomic int quality_override = -2;
static _Atomic int fill_override = -2;

static int quality_property(void) {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {0}; int saved = errno;
    __system_property_get("debug.fp.quality", value);
    int mode = value[0] ? (int)strtoul(value, NULL, 0) & 3 : -1;
    errno = saved;
    return mode;
#else
    return -1;
#endif
}

static int fill_property(void) {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {0}; int saved = errno;
    __system_property_get("debug.fp.extra_fx", value);
    int mode = value[0] ? !!(strtoul(value, NULL, 0) & 4) : -1;
    errno = saved;
    return mode;
#else
    return -1;
#endif
}

/* The frame probe calls this. The fixture never does, so its first read still matches a fixed property. */
void fp_mobile_quality_refresh(void) {
    atomic_store_explicit(&quality_override, quality_property(), memory_order_relaxed);
    atomic_store_explicit(&fill_override, fill_property(), memory_order_relaxed);
}

static unsigned quality_mode(void) {
    int mode = atomic_load_explicit(&quality_override, memory_order_relaxed);
    if (mode == -2) {
        mode = quality_property();
        atomic_store_explicit(&quality_override, mode, memory_order_relaxed);
    }
    if (mode >= 0) return (unsigned)mode;
    return frostpunk_mobile_graphics_reduced(4u) ? 3u : 0u;
}

int fp_mobile_particle_fill_disabled(void) {
    int mode = atomic_load_explicit(&fill_override, memory_order_relaxed);
    if (mode == -2) {
        mode = fill_property();
        atomic_store_explicit(&fill_override, mode, memory_order_relaxed);
    }
    if (mode >= 0) return mode;
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
