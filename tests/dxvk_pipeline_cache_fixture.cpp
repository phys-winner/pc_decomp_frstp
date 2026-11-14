#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "dxvk_pipeline_cache.h"

std::atomic<unsigned> frostpunkPipelineCacheIoFailure{0};
#include "dxvk_pipeline_cache.cpp"

namespace fs = std::filesystem;
static unsigned checks = 0;
static void check(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
static void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
static VkPhysicalDeviceProperties props() {
  VkPhysicalDeviceProperties p = {}; p.vendorID = 0x13b5; p.deviceID = 0xb00b; p.driverVersion = 123;
  for (unsigned i = 0; i < VK_UUID_SIZE; ++i) p.pipelineCacheUUID[i] = uint8_t(i * 11);
  return p;
}
struct FakeCache { std::mutex mutex; uint64_t version = 0; VkDevice owner = VK_NULL_HANDLE; };
static std::mutex gateMutex;
static std::condition_variable gateChanged;
static bool blockData = false, dataEntered = false, releaseData = false;
static std::atomic<unsigned> cacheCreates{0}, cacheDestroys{0}, loaded{0}, snapshots{0}, graphicsCalls{0}, computeCalls{0};
static std::atomic<unsigned> failCreate{0}, incomplete{0};
static std::atomic<bool> oversized{false}, badSnapshot{false};
static FakeCache* unpack(VkPipelineCache cache) { return reinterpret_cast<FakeCache*>(cache); }
static VKAPI_ATTR VkResult VKAPI_CALL fakeCreate(VkDevice device, const VkPipelineCacheCreateInfo* info,
    const VkAllocationCallbacks*, VkPipelineCache* result) {
  require(info->flags == 0, "cache flags must use internal Vulkan synchronization");
  ++cacheCreates;
  if (failCreate.load()) { --failCreate; *result = reinterpret_cast<VkPipelineCache>(uintptr_t(0xbad)); return VK_ERROR_OUT_OF_HOST_MEMORY; }
  auto* cache = new FakeCache;
  cache->owner = device;
  if (info->initialDataSize) {
    require(info->initialDataSize >= 40 && info->pInitialData, "initial data payload");
    std::memcpy(&cache->version, static_cast<const uint8_t*>(info->pInitialData) + 32, 8); ++loaded;
  }
  *result = reinterpret_cast<VkPipelineCache>(cache); return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL fakeDestroy(VkDevice device, VkPipelineCache cache, const VkAllocationCallbacks*) {
  require(uintptr_t(cache) != 0xbad, "undefined failed-create output must not be destroyed");
  require(unpack(cache)->owner == device, "cache destruction owns logical device");
  ++cacheDestroys; delete unpack(cache);
}
static VKAPI_ATTR VkResult VKAPI_CALL fakeData(VkDevice device, VkPipelineCache handle, size_t* size, void* output) {
  ++snapshots;
  require(unpack(handle)->owner == device, "snapshot owns logical device");
  if (!output) { *size = oversized ? 33 * 1024 * 1024 : 40; return VK_SUCCESS; }
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    if (blockData) { dataEntered = true; gateChanged.notify_all(); gateChanged.wait(lock, [] { return releaseData; }); }
  }
  if (incomplete.load()) { --incomplete; return VK_INCOMPLETE; }
  std::lock_guard<std::mutex> lock(unpack(handle)->mutex);
  require(*size >= 40, "data buffer capacity");
  uint8_t bytes[40] = {}; auto p = props();
  auto put = [&](unsigned at, uint32_t value) { std::memcpy(bytes + at, &value, 4); };
  put(0, badSnapshot ? 33 : 32); put(4, 1); put(8, p.vendorID); put(12, p.deviceID);
  std::memcpy(bytes + 16, p.pipelineCacheUUID, 16); std::memcpy(bytes + 32, &unpack(handle)->version, 8);
  std::memcpy(output, bytes, 40); *size = 40; return VK_SUCCESS;
}
static const VkAllocationCallbacks* expectedAllocator;
static const void* expectedInfo;
static uint32_t expectedCount = 1;
static VKAPI_ATTR VkResult VKAPI_CALL fakeGraphics(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* info, const VkAllocationCallbacks* allocator, VkPipeline* result) {
  require(count == expectedCount && info == expectedInfo && allocator == expectedAllocator, "graphics arguments preserved");
  ++graphicsCalls;
  if (cache) { require(unpack(cache)->owner == device, "graphics cache owns logical device"); std::lock_guard<std::mutex> lock(unpack(cache)->mutex); ++unpack(cache)->version; }
  *result = reinterpret_cast<VkPipeline>(uintptr_t(0x1234)); errno = ERANGE;
  return VK_PIPELINE_COMPILE_REQUIRED_EXT;
}
static VKAPI_ATTR VkResult VKAPI_CALL fakeCompute(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo* info, const VkAllocationCallbacks* allocator, VkPipeline* result) {
  require(count == expectedCount && info == expectedInfo && allocator == expectedAllocator, "compute arguments preserved");
  ++computeCalls;
  if (cache) { require(unpack(cache)->owner == device, "compute cache owns logical device"); std::lock_guard<std::mutex> lock(unpack(cache)->mutex); ++unpack(cache)->version; }
  *result = reinterpret_cast<VkPipeline>(uintptr_t(0x5678)); errno = EDOM;
  return VK_SUCCESS;
}
namespace dxvk {
struct FrostpunkPipelineCacheTest {
  static VkPipelineCache handle(const DxvkPipelineCache& c) { return c.m_cache; }
  static std::string name(const DxvkPipelineCache& c) { return c.m_name; }
  static bool save(DxvkPipelineCache& c) { return c.save(); }
  static bool valid(const DxvkPipelineCache& c, const std::vector<uint8_t>& b) { return c.valid(b); }
  static std::vector<uint8_t> envelope(const DxvkPipelineCache& c, const std::vector<uint8_t>& b) { return c.envelope(b); }
  static uint64_t generation(DxvkPipelineCache& c, bool saved) {
    std::lock_guard<std::mutex> lock(c.m_mutex); return saved ? c.m_savedGeneration : c.m_generation;
  }
};
}
using Cache = dxvk::DxvkPipelineCache;
using Test = dxvk::FrostpunkPipelineCacheTest;
static Cache* make(const fs::path& directory, bool enabled = true, VkPhysicalDeviceProperties p = props(), uintptr_t device = 1) {
  return new Cache(reinterpret_cast<VkDevice>(device), fakeCreate, fakeDestroy, fakeData,
    fakeGraphics, fakeCompute, p, directory.string(), enabled);
}
static std::vector<uint8_t> read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}};
}
static void write(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); file.close();
#if !defined(_WIN32)
  chmod(path.c_str(), 0600);
#endif
}
template<class Predicate> static void waitFor(Predicate predicate, const char* reason, unsigned milliseconds = 5000) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (!predicate() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(predicate(), reason);
}
static void graphics(Cache& c) {
  VkGraphicsPipelineCreateInfo info = {}; VkAllocationCallbacks allocator = {}; VkPipeline p;
  expectedInfo = &info; expectedAllocator = &allocator; errno = EINVAL;
  check(c.createGraphicsPipelines(1, &info, &allocator, &p) == VK_PIPELINE_COMPILE_REQUIRED_EXT
    && uintptr_t(p) == 0x1234 && errno == ERANGE, "graphics result/output/errno preserved");
}
static void compute(Cache& c) {
  VkComputePipelineCreateInfo info = {}; VkAllocationCallbacks allocator = {}; VkPipeline p;
  expectedInfo = &info; expectedAllocator = &allocator; errno = EINVAL;
  check(c.createComputePipelines(1, &info, &allocator, &p) == VK_SUCCESS
    && uintptr_t(p) == 0x5678 && errno == EDOM, "compute result/output/errno preserved");
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("one NEW scratch directory argument required");
    fs::path directory = fs::absolute(argv[1]);
    check(!fs::exists(directory), "never overwrite an existing test directory");
    fs::create_directories(directory);
#if !defined(_WIN32)
    chmod(directory.c_str(), 0700);
#endif
    Cache* c = make(directory);
    check(Test::handle(*c) != VK_NULL_HANDLE, "cache created");
    auto path = directory / Test::name(*c);
    graphics(*c); compute(*c);
    waitFor([&] { return Test::generation(*c, true) == Test::generation(*c, false); }, "first dirty saved");
    auto good = read(path);
    check(Test::valid(*c, good), "first snapshot envelope valid");
    delete c;
    unsigned oldLoaded = loaded;
    c = make(directory);
    check(loaded == oldLoaded + 1, "fresh owner loads existing native data before first creation");
    delete c;
    unsigned negative = 0;
    const unsigned mutations[] = {0,8,12,16,20,24,28,32,48,80,88,92,96,100,104};
    c = make(directory);
    for (unsigned offset : mutations) {
      auto bad = good; bad[offset] ^= 1;
      if (offset >= 88) {
        uint64_t sum = 14695981039346656037ull;
        for (size_t i = 88; i < bad.size(); ++i) sum = (sum ^ bad[i]) * 1099511628211ull;
        std::memcpy(bad.data() + 80, &sum, 8);
      }
      check(!Test::valid(*c, bad), "corrupted identity/native header rejected"); ++negative;
    }
    auto shortFile = good; shortFile.pop_back(); check(!Test::valid(*c, shortFile), "truncated rejected"); ++negative;
    auto longFile = good; longFile.push_back(0); check(!Test::valid(*c, longFile), "trailing rejected"); ++negative;
    delete c;
    auto corrupt = good; corrupt[48] ^= 1; write(path, corrupt);
    oldLoaded = loaded; c = make(directory);
    check(loaded == oldLoaded && Test::handle(*c), "corrupt file falls back empty before Vulkan"); delete c;
    write(path, good); failCreate = 1;
    c = make(directory); check(Test::handle(*c) && failCreate == 0, "driver rejects initial cache then empty retry"); delete c;
    failCreate = 2;
    c = make(directory); check(!Test::handle(*c), "both cache failures fall back null"); graphics(*c); delete c;
    auto other = props(); other.vendorID = 0x5143;
    c = make(directory, true, other); check(!Test::handle(*c), "non Mali disabled"); compute(*c); delete c;
    c = make(directory, false); check(!Test::handle(*c), "nonprofile disabled"); delete c;
    c = make(directory / "missing"); check(!Test::handle(*c), "unknown private directory disabled"); delete c;

    c = make(directory);
    for (unsigned failure = 1; failure <= 5; ++failure) {
      frostpunkPipelineCacheIoFailure = failure; errno = EIO;
      check(!Test::save(*c) && read(path) == good, "I/O failure keeps previous valid file");
    }
    frostpunkPipelineCacheIoFailure = 0;
    oversized = true; check(!Test::save(*c) && read(path) == good, "oversize keeps previous file"); oversized = false;
    badSnapshot = true; check(!Test::save(*c) && read(path) == good, "invalid driver header keeps previous file"); badSnapshot = false;
    incomplete = 3; check(!Test::save(*c) && read(path) == good, "bounded snapshot growth keeps previous file");
    incomplete = 2; check(Test::save(*c) && incomplete == 0, "snapshot growth retry succeeds");
    delete c;

    // Freeze the first writer snapshot, issue another compile, then release it.
    // Its generation acknowledgement must not swallow the second dirty event.
    c = make(directory);
    { std::lock_guard<std::mutex> lock(gateMutex); blockData = true; releaseData = false; dataEntered = false; }
    graphics(*c);
    { std::unique_lock<std::mutex> lock(gateMutex);
      check(gateChanged.wait_for(lock, std::chrono::seconds(3), [] { return dataEntered; }), "writer entered snapshot"); }
    compute(*c);
    { std::lock_guard<std::mutex> lock(gateMutex); blockData = false; releaseData = true; }
    gateChanged.notify_all();
    waitFor([&] { return Test::generation(*c, true) >= 1; }, "first generation ack");
    check(Test::generation(*c, true) == 1 && Test::generation(*c, false) == 2, "later generation stays dirty");
    waitFor([&] { return Test::generation(*c, true) == 2; }, "two second periodic followup");
    delete c;

    // Failed writes must wait for cadence, and teardown performs one bounded
    // final retry even when the two-second deadline has not arrived yet.
    c = make(directory); frostpunkPipelineCacheIoFailure = 1; unsigned before = snapshots;
    graphics(*c); waitFor([&] { return snapshots >= before + 2; }, "failed attempt reached I/O");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(snapshots == before + 2, "write failure does not busy loop");
    frostpunkPipelineCacheIoFailure = 0; before = snapshots;
    auto begin = std::chrono::steady_clock::now(); delete c;
    check(snapshots == before + 2 && std::chrono::steady_clock::now() - begin < std::chrono::seconds(1), "teardown immediate final save and join");
    check(read(path) != good, "final save contains later pipeline state");

    c = make(directory);
    Cache* second = make(directory, true, props(), 2);
    check(Test::handle(*c) != Test::handle(*second), "logical devices use separate native cache handles");
    graphics(*c); compute(*second); delete second; delete c;

    // Destruction must wait for the actual in-flight snapshot before destroying
    // its native cache handle, even when stop arrives during vkGet*Data.
    c = make(directory);
    { std::lock_guard<std::mutex> lock(gateMutex); blockData = true; releaseData = false; dataEntered = false; }
    graphics(*c);
    { std::unique_lock<std::mutex> lock(gateMutex);
      check(gateChanged.wait_for(lock, std::chrono::seconds(3), [] { return dataEntered; }), "teardown writer entered snapshot"); }
    std::atomic<bool> destroyed{false}; unsigned oldDestroys = cacheDestroys;
    std::thread teardown([&] { delete c; destroyed = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(!destroyed && cacheDestroys == oldDestroys, "cache handle lives until blocked writer finishes");
    { std::lock_guard<std::mutex> lock(gateMutex); blockData = false; releaseData = true; }
    gateChanged.notify_all(); teardown.join();
    check(destroyed && cacheDestroys == oldDestroys + 1, "writer joined before native cache destruction");
    check(cacheDestroys + 3 == cacheCreates, "every successful cache destroyed; failed outputs ignored");
    unsigned leftovers = 0; for (const auto& entry : fs::directory_iterator(directory)) if (entry.path().filename().string().find(".tmp-") != std::string::npos) ++leftovers;
    check(!leftovers, "temporary files cleaned on injected failures");
    std::printf("{\"success\":true,\"checks\":%u,\"negative_cache_cases\":%u,\"io_failure_cases\":5,\"all_14_routes_source_checked\":true,\"generation_and_teardown_pass\":true,\"vulkan_driver_stubbed\":true}\n", checks, negative);
    fs::remove_all(directory); return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "pipeline cache fixture: %s\n", error.what()); return 1; }
}
