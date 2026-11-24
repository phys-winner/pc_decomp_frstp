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
#include "../../reference/etcpak/ProcessRGB.hpp"

/* Private D3DFORMAT codes of the converted archive (android/host/texture_formats.h). */
enum : uint32_t { FP_DXT1 = 0x31545844, FP_DXT3 = 0x33545844, FP_DXT5 = 0x35545844,
                  FP_ASTC6 = 0x36545341, FP_ETC2_RGB = 0x32435445, FP_ETC2_RGBA = 0x41435445 };
/* Conversion targets of FrostpunkTextures.encodeTarget. */
enum : int { TARGET_ASTC6 = 0, TARGET_ETC2 = 1, TARGET_ETC2_RGBA = 2 };

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

static void decode_block(uint32_t format, const uint8_t* bc, uint8_t* rgba16) {
  if (format == FP_DXT1) bcdec_bc1(bc, rgba16, 16);
  else if (format == FP_DXT3) bcdec_bc2(bc, rgba16, 16);
  else bcdec_bc3(bc, rgba16, 16);
}

/* True when a decoded texel of any mip and face has alpha below 255. */
static bool uses_alpha(const uint8_t* blocks, size_t bytes, uint32_t format) {
  const unsigned blockBytes = format == FP_DXT1 ? 8 : 16;
  std::array<uint8_t, 64> block;
  for (size_t at = 0; at + blockBytes <= bytes; at += blockBytes) {
    const uint8_t* bc = blocks + at;
    if (format == FP_DXT5 && bc[0] == 255 && bc[1] == 255) continue;
    if (format == FP_DXT1 && u16(bc) > u16(bc + 2)) continue;   /* four-colour block: opaque */
    decode_block(format, bc, block.data());
    for (unsigned i = 3; i < 64; i += 4) if (block[i] != 255) return true;
  }
  return false;
}

/* ETC2 RGB8 (8 bytes per 4x4 block) or RGBA8 with EAC alpha (16 bytes), the same header and mip order.
 * The alpha variant follows the content unless the caller forces it: every element of one texture
 * array has to keep one format, and the caller knows the arrays. */
static std::vector<uint8_t> convert_etc2(const uint8_t* data, size_t size, unsigned threads, bool forceAlpha,
                                         uint32_t width, uint32_t trueHeight, uint32_t format, uint32_t count, unsigned faces) {
  const bool alpha = forceAlpha || uses_alpha(data + 144, size - 144, format);
  const unsigned outBytes = alpha ? 16 : 8, blockBytes = format == FP_DXT1 ? 8 : 16;
  std::vector<uint8_t> output(data, data + 144);
  put32(output.data() + 8, alpha ? FP_ETC2_RGBA : FP_ETC2_RGB);
  size_t offset = 144;
  for (unsigned mip = 0; mip < count; ++mip) {
    unsigned w = u16(data + 16 + mip * 8), h = u16(data + 18 + mip * 8);
    const unsigned shift = count - 1 - mip, bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t faceBytes = size_t(bw) * bh * blockBytes, encoded = size_t(bw) * bh * outBytes;
    need(w == std::max(1u, width >> shift) && h == std::max(1u, trueHeight >> shift)
      && u32(data + 20 + mip * 8) == faceBytes, "Invalid mip dimensions, order or byte length");
    need(faceBytes <= (size - offset) / faces, "Truncated texture mip");
    need(size_t(bw) * bh * 64 <= (512u << 20), "Texture exceeds the conversion memory limit");
    put32(output.data() + 20 + mip * 8, uint32_t(encoded));
    for (unsigned face = 0; face < faces; ++face) {
      /* The BC blocks already cover the padded 4x4 grid, so their own edge texels fill it. */
      const size_t pitch = size_t(bw) * 4;
      std::vector<uint32_t> bgra(pitch * bh * 4);
      size_t at = output.size(); output.resize(at + encoded);
      const uint8_t* source = data + offset;
      uint8_t* target = output.data() + at;
      auto rows = [&](unsigned first, unsigned last) {
        std::array<uint8_t, 64> block;
        for (unsigned by = first; by < last; ++by) for (unsigned bx = 0; bx < bw; ++bx) {
          decode_block(format, source + (size_t(by) * bw + bx) * blockBytes, block.data());
          for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
            const uint8_t* p = block.data() + (y * 4 + x) * 4;
            bgra[(size_t(by) * 4 + y) * pitch + bx * 4 + x] = uint32_t(p[2]) | uint32_t(p[1]) << 8 | uint32_t(p[0]) << 16 | uint32_t(p[3]) << 24;
          }
        }
        if (last == first) return;
        auto* out = reinterpret_cast<uint64_t*>(target + size_t(first) * bw * outBytes);
        if (alpha) CompressEtc2Rgba(bgra.data() + size_t(first) * 4 * pitch, out, (last - first) * bw, pitch, true);
        else CompressEtc2Rgb(bgra.data() + size_t(first) * 4 * pitch, out, (last - first) * bw, pitch, true);
      };
      const unsigned parts = bh >= 16 ? std::min(threads, bh) : 1;
      std::vector<std::thread> workers;
      try {
        for (unsigned part = 1; part < parts; ++part)
          workers.emplace_back(rows, unsigned(uint64_t(bh) * part / parts), unsigned(uint64_t(bh) * (part + 1) / parts));
      } catch (...) { for (auto& worker : workers) worker.join(); throw; }
      rows(0, bh / parts);
      for (auto& worker : workers) worker.join();
      offset += faceBytes;
    }
  }
  need(offset == size, "Unexpected texture payload tail");
  return output;
}

static std::vector<uint8_t> convert(const uint8_t* data, size_t size, unsigned threads = 1, int target = TARGET_ASTC6) {
  need(threads >= 1 && threads <= 4, "Invalid ASTC encoder thread count");
  need(size >= 144 && size <= (256u << 20), "Invalid texture entry size");
  const uint32_t width = u32(data), height = u32(data + 4), format = u32(data + 8), count = u32(data + 12);
  const unsigned faces = height == UINT32_MAX ? 6 : 1;
  const uint32_t trueHeight = faces == 6 ? width : height;
  need(width && trueHeight && width <= 16384 && trueHeight <= 16384 && count && count <= 16,
       "Invalid texture dimensions or mip count");
  need(format == 0x31545844 || format == 0x33545844 || format == 0x35545844, "Expected DXT1, DXT3 or DXT5");
  need((std::max(width, trueHeight) >> (count - 1)) != 0, "Excess texture mip count");
  need(target == TARGET_ASTC6 || target == TARGET_ETC2 || target == TARGET_ETC2_RGBA, "Unknown texture conversion target");
  if (target != TARGET_ASTC6)
    return convert_etc2(data, size, threads, target == TARGET_ETC2_RGBA, width, trueHeight, format, count, faces);
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
static jbyteArray encode_target(JNIEnv* env, jbyteArray input, jint threads, jint target) {
  try {
    need(input != nullptr, "Missing texture payload");
    jsize size = env->GetArrayLength(input);
    need(size >= 144 && size <= (256 << 20), "Invalid texture entry size");
    std::vector<uint8_t> bytes(size);
    env->GetByteArrayRegion(input, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    if (env->ExceptionCheck()) return nullptr;
    auto output = convert(bytes.data(), bytes.size(), (unsigned)threads, target);
    jbyteArray result = env->NewByteArray(jsize(output.size()));
    if (result) env->SetByteArrayRegion(result, 0, jsize(output.size()), reinterpret_cast<const jbyte*>(output.data()));
    return result;
  } catch (const std::exception& failure) {
    env->ThrowNew(env->FindClass("java/io/IOException"), failure.what());
    return nullptr;
  }
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_frostpunk_staticrecomp_launcher_FrostpunkTextures_encode(JNIEnv* env, jclass, jbyteArray input, jint threads) {
  return encode_target(env, input, threads, TARGET_ASTC6);
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_frostpunk_staticrecomp_launcher_FrostpunkTextures_encodeTarget(JNIEnv* env, jclass, jbyteArray input, jint threads, jint target) {
  return encode_target(env, input, threads, target);
}
#else
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../../reference/etcpak/Decode.hpp"

/* Decoded RGBA of the largest mip (the last one), face 0, on the padded 4x4 grid. */
static std::vector<uint8_t> top_mip(const std::vector<uint8_t>& entry, unsigned& pw, unsigned& ph) {
  const uint8_t* data = entry.data();
  const uint32_t format = u32(data + 8), count = u32(data + 12);
  const unsigned faces = u32(data + 4) == UINT32_MAX ? 6 : 1;
  size_t offset = 144;
  for (unsigned mip = 0; mip + 1 < count; ++mip) offset += size_t(u32(data + 20 + mip * 8)) * faces;
  const unsigned w = u16(data + 16 + (count - 1) * 8), h = u16(data + 18 + (count - 1) * 8);
  pw = (w + 3) & ~3u; ph = (h + 3) & ~3u;
  std::vector<uint8_t> rgba(size_t(pw) * ph * 4);
  need(offset + u32(data + 20 + (count - 1) * 8) <= entry.size(), "Truncated top mip");
  if (format == FP_ETC2_RGB || format == FP_ETC2_RGBA) {
    std::vector<uint32_t> bgra(size_t(pw) * ph);   /* named after the encoder side; see below */
    if (format == FP_ETC2_RGB) DecodeRGB(reinterpret_cast<const uint64_t*>(data + offset), bgra.data(), int32_t(pw), int32_t(ph));
    else DecodeRGBA(reinterpret_cast<const uint64_t*>(data + offset), bgra.data(), int32_t(pw), int32_t(ph));
    for (size_t i = 0; i < bgra.size(); ++i) {
      /* etcpak decodes to R in the low byte; its encoders take B there. */
      rgba[i * 4] = uint8_t(bgra[i]); rgba[i * 4 + 1] = uint8_t(bgra[i] >> 8); rgba[i * 4 + 2] = uint8_t(bgra[i] >> 16);
      rgba[i * 4 + 3] = format == FP_ETC2_RGB ? 255 : uint8_t(bgra[i] >> 24);
    }
  } else if (format == FP_ASTC6) {
    astcenc_config config;
    need(astcenc_config_init(ASTCENC_PRF_LDR, 6, 6, 1, ASTCENC_PRE_FAST, 0, &config) == ASTCENC_SUCCESS, "astc config");
    astcenc_context* context = nullptr;
    need(astcenc_context_alloc(&config, 1, &context, nullptr) == ASTCENC_SUCCESS, "astc context");
    std::vector<uint8_t> plain(size_t(w) * h * 4);
    void* slice = plain.data();
    astcenc_image image{w, h, 1, ASTCENC_TYPE_U8, &slice};
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    need(astcenc_decompress_image(context, data + offset, u32(data + 20 + (count - 1) * 8), &image, &swizzle, 0) == ASTCENC_SUCCESS, "astc decode");
    astcenc_context_free(context);
    for (unsigned y = 0; y < ph; ++y) for (unsigned x = 0; x < pw; ++x)
      std::memcpy(rgba.data() + (size_t(y) * pw + x) * 4, plain.data() + (size_t(std::min(y, h - 1)) * w + std::min(x, w - 1)) * 4, 4);
  } else {
    const unsigned blockBytes = format == FP_DXT1 ? 8 : 16;
    std::array<uint8_t, 64> block;
    for (unsigned by = 0; by < ph / 4; ++by) for (unsigned bx = 0; bx < pw / 4; ++bx) {
      decode_block(format, data + offset + (size_t(by) * (pw / 4) + bx) * blockBytes, block.data());
      for (unsigned y = 0; y < 4; ++y) std::memcpy(rgba.data() + ((size_t(by) * 4 + y) * pw + bx * 4) * 4, block.data() + y * 16, 16);
    }
  }
  return rgba;
}
/* PSNR of the colour channels and of alpha over the visible w x h area. */
static void psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, unsigned pw, unsigned w, unsigned h, double& colour, double& alpha) {
  double sc = 0, sa = 0;
  for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) for (unsigned c = 0; c < 4; ++c) {
    const double d = double(a[(size_t(y) * pw + x) * 4 + c]) - b[(size_t(y) * pw + x) * 4 + c];
    (c == 3 ? sa : sc) += d * d;
  }
  sc /= double(w) * h * 3; sa /= double(w) * h;
  colour = sc == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / sc);
  alpha = sa == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / sa);
}
/* Real archive entries given as files: quality and time of both targets against the BC source. */
static int measure(int files, char** names) {
  for (int i = 0; i < files; ++i) {
    FILE* file = std::fopen(names[i], "rb");
    if (!file) { std::printf("TEXTURE_FILE %s unreadable\n", names[i]); return 1; }
    std::vector<uint8_t> entry; uint8_t buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, file)) > 0;) entry.insert(entry.end(), buffer, buffer + n);
    std::fclose(file);
    need(entry.size() >= 144, "Short texture entry");
    const uint32_t count = u32(entry.data() + 12);
    const unsigned w = u16(entry.data() + 16 + (count - 1) * 8), h = u16(entry.data() + 18 + (count - 1) * 8);
    unsigned pw, ph; auto source = top_mip(entry, pw, ph);
    std::printf("TEXTURE_FILE %s %.4s %ux%u mips=%u", names[i], reinterpret_cast<const char*>(entry.data() + 8), w, h, count);
    for (int target : {TARGET_ETC2, TARGET_ETC2_RGBA, TARGET_ASTC6}) {
      auto t0 = std::chrono::steady_clock::now();
      auto result = convert(entry.data(), entry.size(), 4, target);
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      unsigned qw, qh; auto decoded = top_mip(result, qw, qh);
      double colour, alpha; psnr(source, decoded, pw, w, h, colour, alpha);
      std::printf(" | %.4s bytes=%zu psnr_rgb=%.2f psnr_a=%.2f ms=%.0f", reinterpret_cast<const char*>(result.data() + 8), result.size(), colour, alpha, ms);
    }
    std::printf("\n");
  }
  return 0;
}
/* --convert TARGET INPUT OUTPUT: one archive entry through convert(), for a check with another decoder. */
static int convert_file(int target, const char* input, const char* output) {
  FILE* file = std::fopen(input, "rb");
  if (!file) return 1;
  std::vector<uint8_t> entry; uint8_t buffer[65536];
  for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, file)) > 0;) entry.insert(entry.end(), buffer, buffer + n);
  std::fclose(file);
  auto result = convert(entry.data(), entry.size(), 2, target);
  file = std::fopen(output, "wb");
  if (!file) return 1;
  const bool ok = std::fwrite(result.data(), 1, result.size(), file) == result.size();
  return std::fclose(file) || !ok;
}
int main(int argc, char** argv) {
  if (argc == 5 && !std::strcmp(argv[1], "--convert")) return convert_file(std::atoi(argv[2]), argv[3], argv[4]);
  if (argc > 1) return measure(argc - 1, argv + 1);
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
    for (int target : {TARGET_ETC2, TARGET_ETC2_RGBA}) {
      /* Zero blocks: DXT1 decodes to opaque black (RGB8), DXT3 and DXT5 to alpha 0 (RGBA8). */
      const bool alpha = target == TARGET_ETC2_RGBA || format != 0x31545844;
      auto etc = convert(source.data(), source.size(), 1, target);
      assert(etc.size() == 144 + 5 * (alpha ? 16u : 8u) * faces);
      assert(u32(etc.data() + 8) == (alpha ? FP_ETC2_RGBA : FP_ETC2_RGB));
      assert(u32(etc.data() + 20) == (alpha ? 16u : 8u) && u32(etc.data() + 28) == (alpha ? 64u : 32u));
    }
    source.pop_back();
    try { convert(source.data(), source.size()); assert(false); } catch (const std::runtime_error&) { }
    try { convert(source.data(), source.size(), 1, TARGET_ETC2); assert(false); } catch (const std::runtime_error&) { }
  }
  /* A smooth 64x64 DXT5 image with an alpha ramp, one mip: decode and compare both ETC2 variants. */
  {
    const unsigned edge = 64, blocks = edge / 4;
    std::vector<uint8_t> source(144 + blocks * blocks * 16);
    put32(source.data(), edge); put32(source.data() + 4, edge); put32(source.data() + 8, FP_DXT5); put32(source.data() + 12, 1);
    uint16_t e = edge; std::memcpy(source.data() + 16, &e, 2); std::memcpy(source.data() + 18, &e, 2);
    put32(source.data() + 20, blocks * blocks * 16);
    for (unsigned by = 0; by < blocks; ++by) for (unsigned bx = 0; bx < blocks; ++bx) {
      uint8_t* b = source.data() + 144 + (by * blocks + bx) * 16;
      b[0] = uint8_t(255 - bx * 12); b[1] = uint8_t(b[0] - 24);          /* alpha0 > alpha1: eight-step ramp */
      const uint64_t codes = 0x0000a2c8d1a2c8d1ull;                       /* a fixed mix of ramp steps */
      for (unsigned i = 0; i < 6; ++i) b[2 + i] = uint8_t(codes >> (8 * i));
      const unsigned r = 4 + bx + by / 2, g = 8 + by * 3, bl = 6 + bx;    /* 5:6:5 */
      const uint16_t c0 = uint16_t((r + 3) << 11 | (g + 6) << 5 | (bl + 3)), c1 = uint16_t(r << 11 | g << 5 | bl);
      std::memcpy(b + 8, &c0, 2); std::memcpy(b + 10, &c1, 2);
      put32(b + 12, 0xb4e11eb4u ^ (bx * 0x01010101u));
    }
    unsigned pw, ph; auto reference = top_mip(source, pw, ph);
    auto automatic = convert(source.data(), source.size(), 2, TARGET_ETC2);
    assert(u32(automatic.data() + 8) == FP_ETC2_RGBA && automatic.size() == source.size());
    auto decoded = top_mip(automatic, pw, ph);
    double colour, alpha; psnr(reference, decoded, pw, edge, edge, colour, alpha);
    std::printf("TEXTURE_ETC2 synthetic DXT5 64x64 psnr_rgb=%.2f psnr_a=%.2f\n", colour, alpha);
    assert(colour >= 30 && alpha >= 36);
    assert(convert(source.data(), source.size(), 1, TARGET_ETC2) == automatic);   /* thread count does not change the bytes */
    for (unsigned i = 0; i < blocks * blocks; ++i) { source[144 + i * 16] = 255; source[145 + i * 16] = 255; }
    auto opaque = convert(source.data(), source.size(), 2, TARGET_ETC2);
    assert(u32(opaque.data() + 8) == FP_ETC2_RGB && opaque.size() == 144 + blocks * blocks * 8);
    reference = top_mip(source, pw, ph); decoded = top_mip(opaque, pw, ph);
    psnr(reference, decoded, pw, edge, edge, colour, alpha);
    std::printf("TEXTURE_ETC2 synthetic opaque DXT5 64x64 psnr_rgb=%.2f psnr_a=%.2f\n", colour, alpha);
    assert(colour >= 30 && alpha == 99);
    assert(u32(convert(source.data(), source.size(), 1, TARGET_ETC2_RGBA).data() + 8) == FP_ETC2_RGBA);
  }
  puts("TEXTURE_CONVERSION DXT1_DXT3_DXT5 mip_order cube_faces truncated_rejected ETC2_RGB ETC2_RGBA PASS");
}
#endif
