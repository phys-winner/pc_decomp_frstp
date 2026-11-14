#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <future>
#include <thread>
#include "../../reference/astcenc/Source/astcenc.h"
#define BCDECDEF static inline
#define BCDEC_IMPLEMENTATION
#include "../../reference/bcdec/bcdec.h"

static uint32_t u32(const uint8_t* p) { uint32_t n; std::memcpy(&n, p, 4); return n; }
static uint16_t u16(const uint8_t* p) { uint16_t n; std::memcpy(&n, p, 2); return n; }
static void put32(uint8_t* p, uint32_t n) { std::memcpy(p, &n, 4); }
static void need(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

static unsigned mip_skip(uint32_t key, uint32_t width, uint32_t height, unsigned count) {
  static const struct { uint32_t key; uint16_t width, height; uint8_t skip; } caps[] = {
#include "../host/switch_texture_caps.inc"
  };
  auto cap = std::lower_bound(std::begin(caps), std::end(caps), key,
      [](const auto& row, uint32_t value) { return row.key < value; });
  return cap != std::end(caps) && cap->key == key && cap->width == width && cap->height == height
      && count > cap->skip && count <= 16 ? cap->skip : 0;
}

static std::vector<uint8_t> convert(const uint8_t* data, size_t size, unsigned threads = 1) {
  need(threads >= 1 && threads <= 4, "Invalid ASTC encoder thread count");
  need(size >= 144 && size <= (256u << 20), "Invalid texture entry size");
  const uint32_t width = u32(data), height = u32(data + 4), format = u32(data + 8), count = u32(data + 12);
  const unsigned faces = height == UINT32_MAX ? 6 : 1;
  const uint32_t trueHeight = faces == 6 ? width : height;
  need(width && trueHeight && width <= 16384 && trueHeight <= 16384 && count && count <= 16,
       "Invalid texture dimensions or mip count");
  need(format == 0x31545844 || format == 0x33545844 || format == 0x35545844, "Expected DXT1, DXT3 or DXT5");
  need((std::max(width, trueHeight) >> (count - 1)) != 0, "Excess texture mip count");
  astcenc_config config;
  need(astcenc_config_init(ASTCENC_PRF_LDR, 6, 6, 1, ASTCENC_PRE_FAST, 0, &config) == ASTCENC_SUCCESS,
       "Cannot configure ASTC encoder");
  astcenc_context* raw = nullptr;
  need(astcenc_context_alloc(&config, threads, &raw, nullptr) == ASTCENC_SUCCESS, "Cannot allocate ASTC encoder");
  std::unique_ptr<astcenc_context, decltype(&astcenc_context_free)> context(raw, astcenc_context_free);
  const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  std::vector<uint8_t> output(data, data + 144);
  put32(output.data() + 8, 0x36545341);
  size_t offset = 144;
  for (unsigned mip = 0; mip < count; ++mip) {
    unsigned w = u16(data + 16 + mip * 8), h = u16(data + 18 + mip * 8);
    const unsigned shift = count - 1 - mip, blockBytes = format == 0x31545844 ? 8 : 16;
    const size_t faceBytes = size_t((w + 3) / 4) * ((h + 3) / 4) * blockBytes;
    need(w == std::max(1u, width >> shift) && h == std::max(1u, trueHeight >> shift)
      && u32(data + 20 + mip * 8) == faceBytes, "Invalid mip dimensions, order or byte length");
    need(faceBytes <= (size - offset) / faces, "Truncated texture mip");
    need(size_t(w) * h * 4 <= (512u << 20), "Texture exceeds the conversion memory limit");
    const size_t encoded = size_t((w + 5) / 6) * ((h + 5) / 6) * 16;
    put32(output.data() + 20 + mip * 8, uint32_t(encoded));
    for (unsigned face = 0; face < faces; ++face) {
      std::vector<uint8_t> rgba(size_t(w) * h * 4);
      std::array<uint8_t, 64> block;
      for (unsigned y = 0; y < h; y += 4) for (unsigned x = 0; x < w; x += 4) {
        const uint8_t* bc = data + offset + (size_t(y / 4) * ((w + 3) / 4) + x / 4) * blockBytes;
        if (format == 0x31545844) bcdec_bc1(bc, block.data(), 16);
        else if (format == 0x33545844) bcdec_bc2(bc, block.data(), 16);
        else bcdec_bc3(bc, block.data(), 16);
        for (unsigned row = 0; row < std::min(4u, h - y); ++row)
          std::memcpy(rgba.data() + (size_t(y + row) * w + x) * 4,
                      block.data() + row * 16, std::min(4u, w - x) * 4);
      }
      offset += faceBytes;
      void* slice = rgba.data();
      astcenc_image image{w, h, 1, ASTCENC_TYPE_U8, &slice};
      size_t at = output.size(); output.resize(at + encoded);
      std::vector<astcenc_error> results(threads, ASTCENC_SUCCESS);
      std::vector<std::thread> workers;
      std::promise<bool> begin; auto ready = begin.get_future().share();
      // Do not enter the codec until every worker exists; a failed thread creation must not strand its peers.
      try {
        for (unsigned index = 1; index < threads; ++index) workers.emplace_back([&, index, ready] {
          if (ready.get()) results[index] = astcenc_compress_image(context.get(), &image, &swizzle, output.data() + at, encoded, index);
        });
      } catch (...) {
        begin.set_value(false); for (auto& worker : workers) worker.join(); throw;
      }
      begin.set_value(true);
      results[0] = astcenc_compress_image(context.get(), &image, &swizzle, output.data() + at, encoded, 0);
      for (auto& worker : workers) worker.join();
      for (auto result : results) need(result == ASTCENC_SUCCESS, "ASTC compression failed");
      astcenc_compress_reset(context.get());
    }
  }
  need(offset == size, "Unexpected texture payload tail");
  return output;
}

#ifndef FP_TEXTURE_TEST
#include <jni.h>
extern "C" JNIEXPORT jint JNICALL
Java_com_frostpunk_staticrecomp_launcher_FrostpunkTextures_mipSkip(JNIEnv*, jclass, jint key, jint width, jint height, jint count) {
  return (jint)mip_skip((uint32_t)key, (uint32_t)width, (uint32_t)height, (unsigned)count);
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_frostpunk_staticrecomp_launcher_FrostpunkTextures_encode(JNIEnv* env, jclass, jbyteArray input, jint threads) {
  try {
    need(input != nullptr, "Missing texture payload");
    jsize size = env->GetArrayLength(input);
    need(size >= 144 && size <= (256 << 20), "Invalid texture entry size");
    std::vector<uint8_t> bytes(size);
    env->GetByteArrayRegion(input, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    if (env->ExceptionCheck()) return nullptr;
    auto output = convert(bytes.data(), bytes.size(), (unsigned)threads);
    jbyteArray result = env->NewByteArray(jsize(output.size()));
    if (result) env->SetByteArrayRegion(result, 0, jsize(output.size()), reinterpret_cast<const jbyte*>(output.data()));
    return result;
  } catch (const std::exception& failure) {
    env->ThrowNew(env->FindClass("java/io/IOException"), failure.what());
    return nullptr;
  }
}
#else
#undef NDEBUG
#include <cassert>
#include <cstdio>
int main() {
  assert(mip_skip(0x00323991u, 1024, 1024, 11) == 2);
  assert(mip_skip(0x00323991u, 512, 512, 10) == 0);
  assert(mip_skip(0x00323991u, 1024, 1024, 2) == 0);
  assert(mip_skip(0xffffffffu, 1024, 1024, 11) == 0);
  for (uint32_t format : {0x31545844u, 0x33545844u, 0x35545844u}) for (unsigned faces : {1u, 6u}) {
    unsigned bytes = format == 0x31545844 ? 8 : 16;
    std::vector<uint8_t> source(144 + 5 * bytes * faces);
    put32(source.data(), 8); put32(source.data() + 4, faces == 6 ? UINT32_MAX : 8);
    put32(source.data() + 8, format); put32(source.data() + 12, 2);
    uint16_t four = 4, eight = 8;
    std::memcpy(source.data() + 16, &four, 2); std::memcpy(source.data() + 18, &four, 2);
    put32(source.data() + 20, bytes);
    std::memcpy(source.data() + 24, &eight, 2); std::memcpy(source.data() + 26, &eight, 2);
    put32(source.data() + 28, 4 * bytes);
    auto result = convert(source.data(), source.size());
    assert(result.size() == 144 + 80 * faces && u32(result.data() + 8) == 0x36545341);
    assert(u32(result.data() + 20) == 16 && u32(result.data() + 28) == 64);
    source.pop_back();
    try { convert(source.data(), source.size()); assert(false); } catch (const std::runtime_error&) { }
  }
  puts("TEXTURE_CONVERSION DXT1_DXT3_DXT5 mip_order cube_faces truncated_rejected PASS");
}
#endif
