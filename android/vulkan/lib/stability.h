/* Stability guards of the native device (stability.cpp): safe mode across launches, the crash
 * report, driver serialization and the fallback logs.  Ideas from the external ports collected in
 * external-theories (T02, T05, T10); the implementation is ours. */
#pragma once
#include <cstdint>
#include "vk.h"

namespace rdvk {
/* Reads files/rdvk-guard.txt and installs the crash report.  0: the native renderer crashed in
 * two sessions in a row (this build) -> decline, the game keeps D3D11/DXVK. */
int guard_init();
/* 0 normal; 1 after one crashed session: driver compiler calls serialized on every GPU. */
int guard_level();
/* STOP of the library: counts as a crash of the native renderer. */
void guard_fatal();
/* The driver lacks what the device needs (Vulkan 1.3, dynamic rendering...): the next launch
 * declines at once instead of after two crashes. */
void guard_unsupported();
/* Once per present: a long stable session resets the crash counter. */
void guard_present(uint64_t frames);
/* One mutex around the driver calls that run its shader compiler (PowerVR is not thread safe). */
void serialize_driver(const char* reason);
/* First occurrence of a key, for fallback logs. */
bool first_time(const char* key);
/* Logs (once per format and usage) features the driver lacks for an image or vertex format. */
void check_image_format(VkFormat format, VkImageUsageFlags usage);
void check_vertex_format(VkFormat format);
}  // namespace rdvk
