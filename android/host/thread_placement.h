#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Called once on the guest main thread before the game starts. */
void fp_thread_placement_main(void);
/* Called on the render thread at every Present; see thread_placement.c. */
void frostpunk_thread_placement_frame(void);
#ifdef __cplusplus
}
#endif
