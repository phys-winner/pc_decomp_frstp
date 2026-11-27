/* libfrostpunk_rdvk.so internals shared by all translation units. */
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include "rdvk_abi.h"
#include "rdvk_hooks.h"

namespace rdvk {
extern const rdvk_host* host;
void log(const char* format, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void stop(const char* format, ...) __attribute__((format(printf, 1, 2)));
/* The selected renderer failed (renderer_failure.h): device_lost false, it did not start (driver refused, device or
   swapchain creation failed, a feature missing) before the first presented frame; true, the GPU device was lost.
   Records it for the launcher, which offers the next renderer, and ends the game like stop(). */
[[noreturn]] void renderer_stop(bool device_lost, const char* format, ...) __attribute__((format(printf, 2, 3)));
void renderer_presented();          /* after a present that returned VK_SUCCESS or VK_SUBOPTIMAL_KHR */
bool renderer_has_presented();
const char* gpu_failure_note();     /* why Gpu::init refused the driver (its last RDVK_GPU line) */
void renderer_qa_start();           /* debug.fp.renderer_fail=start:<id>: before the device is created */
void renderer_qa_frame();           /* debug.fp.renderer_fail=lost:<id>: 60 frames after the first present */
std::string property(const char* name);

/* Native mode: fills the hook table; 0 keeps the D3D11/DXVK backend. */
int native_init(rdvk_fn table[RDVK_HOOK_MAX]);
/* COM call on one of our guest proxies; 1 when handled (the caller pops the return address). */
int com_call(CPU* c, uint64_t target);
void com_census_frame();
/* RGBA8 copy of a 2D texture for the game's photo (render thread); 0 or an HRESULT. */
uint32_t read_texture(uint64_t texture, uint32_t* width, uint32_t* height, uint32_t* format, uint8_t** rgba);

/* Guest memory is identity mapped (win64_pointer): guest addresses are host addresses. */
template <class T> inline T rd(uint64_t address) { T v; memcpy(&v, reinterpret_cast<const void*>(address), sizeof(T)); return v; }
template <class T> inline void wr(uint64_t address, T v) { memcpy(reinterpret_cast<void*>(address), &v, sizeof(T)); }
inline uint64_t rd64(uint64_t a) { return rd<uint64_t>(a); }
inline uint32_t rd32(uint64_t a) { return rd<uint32_t>(a); }
inline uint16_t rd16(uint64_t a) { return rd<uint16_t>(a); }
inline uint8_t rd8(uint64_t a) { return rd<uint8_t>(a); }
inline void wr64(uint64_t a, uint64_t v) { wr<uint64_t>(a, v); }
inline void wr32(uint64_t a, uint32_t v) { wr<uint32_t>(a, v); }
inline void wr8(uint64_t a, uint8_t v) { wr<uint8_t>(a, v); }

/* Win64 arguments of a hooked function: rcx, rdx, r8, r9, then the stack above the return
 * address and the 32-byte shadow space. */
inline uint64_t arg(CPU* c, unsigned i) {
    switch (i) { case 0: return c->rcx; case 1: return c->rdx; case 2: return c->r8; case 3: return c->r9; default: break; }
    return rd64(c->rsp + 40 + (i - 4) * 8);
}
inline float argf(CPU* c, unsigned i) {
    if (i < 4) { float f; memcpy(&f, &c->xmm[i], 4); return f; }
    uint32_t bits = uint32_t(arg(c, i)); float f; memcpy(&f, &bits, 4); return f;
}
/* Return from the hooked function: value in rax, pop the guest return address. */
inline void ret(CPU* c, uint64_t value) { c->rax = value; c->rsp += 8; }
inline void ret(CPU* c) { c->rsp += 8; }
inline void retf(CPU* c, float value) { memset(&c->xmm[0], 0, 16); memcpy(&c->xmm[0], &value, 4); c->rsp += 8; }

/* Call a guest function (engine allocator, engine helpers) from native code on the caller's
 * guest stack.  Volatile registers follow the Win64 ABI; the result is rax. */
uint64_t guest_call(CPU* c, uint64_t va, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0);
}  // namespace rdvk
