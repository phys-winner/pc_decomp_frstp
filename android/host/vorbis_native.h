#pragma once
#include "cpu64.h"
/* Native replacement for a translated libvorbis function at `target`, or NULL (not replaced,
 * disabled by debug.fp.vorbis_native=0, or disabled after a self-check mismatch). */
void (*fp_vorbis_native_lookup(uint64_t target))(CPU *);
