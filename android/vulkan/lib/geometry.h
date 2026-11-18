/* Mobile geometry on the native Vulkan device: simplified index data for static meshes, the method
 * of android/dxvk/mobile_geometry.inc (see its header) on the device's own objects. */
#pragma once
#include <atomic>
#include <cstdint>
#include "d3d.h"

namespace rdvk {
struct Object;
struct Buffer;
struct InputLayout;

namespace geometry {
extern std::atomic<bool> drawing;   /* draws may get shorter index counts */
void init();
void buffer_created(Buffer* buffer, const d3d::BUFFER_DESC& desc, const void* data);
void layout_created(InputLayout* layout, const d3d::INPUT_ELEMENT_DESC* elements, uint32_t count);
void forget(const Object* object);
/* The index count a DrawIndexed of [start, start+count) of the bound index buffer is recorded with. */
uint32_t draw(uint32_t count, uint32_t start, int32_t base);
/* A draw whose range comes from a GPU buffer. */
void draw_unknown_range();
/* The game writes into a buffer (UpdateSubresource, CopyResource, CopySubresourceRegion). */
void written(const Buffer* buffer);
void present();
}  // namespace geometry
}  // namespace rdvk
