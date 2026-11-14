#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Raw accepted queue presents, not compositor delivery. frame_id=0/result=1
 * retires a presenter; successful frame_id=0 repeats never contribute. */
void frostpunk_frame_profile_present(const void *presenter, uint64_t frame_id,
                                    uint64_t monotonic_ns, int32_t result);
/* Deterministic flush also permits an explicit end-of-observation snapshot.
 * Missing live-tail coverage must not be treated as zero elapsed time. */
void fp_frame_profile_flush(uint64_t monotonic_ns);
void fp_frame_profile_stop(void);

#ifdef __cplusplus
}
#endif
