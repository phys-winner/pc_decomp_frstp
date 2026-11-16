/* Mesh level of detail for small render targets.
 *
 * The engine does not pick a level of detail per object.  MeshEntity keeps one global level
 * (0 = full detail .. 3), and the camera sets it from the "LOD set by level" value of the current
 * zoom step (0x140FBA350, meshentity.cpp).  Those values were tuned for a desktop picture: with
 * the scene rendered at a fraction of that height the same meshes cover a fraction of the pixels,
 * while the vertex work stays the same.  A developed city draws about 8 million indices per frame
 * in the G-buffer pass, and on a tile-based GPU that vertex work does not shrink with the output
 * resolution.
 *
 * The wrapper raises the requested level to a minimum; a mesh without that level keeps its
 * coarsest one (the original clamps).  debug.fp.lod=0..3 forces the minimum for measurements.
 */
#include "cpu64.h"
#include "mobile_lod.h"
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#endif

void fp_lod_original_L_000140FBA350(CPU *);

static _Atomic unsigned policy_minimum;

void fp_mobile_lod_minimum(unsigned level) { atomic_store_explicit(&policy_minimum, level > 3 ? 3 : level, memory_order_relaxed); }

static _Atomic int lod_override = -2;

static int lod_property(void) {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {0}; int saved = errno;
    __system_property_get("debug.fp.lod", value);
    int mode = value[0] >= '0' && value[0] <= '3' && !value[1] ? value[0] - '0' : -1;
    errno = saved;
    return mode;
#else
    return -1;
#endif
}

void fp_mobile_lod_refresh(void) {
    atomic_store_explicit(&lod_override, lod_property(), memory_order_relaxed);
}

unsigned fp_mobile_lod_level(unsigned requested) {
    unsigned minimum = atomic_load_explicit(&policy_minimum, memory_order_relaxed);
    int mode = atomic_load_explicit(&lod_override, memory_order_relaxed);
    if (mode == -2) {
        mode = lod_property();
        atomic_store_explicit(&lod_override, mode, memory_order_relaxed);
    }
    if (mode >= 0) minimum = (unsigned)mode;
    /* Out-of-range requests reach the original unchanged: it reports them itself. */
    return requested < 4 && requested < minimum ? minimum : requested;
}

/* void MeshEntity::SetGlobalLOD(unsigned level): ecx = level. */
void L_000140FBA350(CPU *c) {
    c->rcx = fp_mobile_lod_level((uint32_t)c->rcx);
    fp_lod_original_L_000140FBA350(c);
}
