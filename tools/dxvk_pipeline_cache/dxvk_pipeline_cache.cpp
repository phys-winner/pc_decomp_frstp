#include "dxvk_pipeline_cache.h"
#include "dxvk_pipeline_cache_identity.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#if defined(FP_PIPELINE_CACHE_TEST)
#include <atomic>
extern std::atomic<unsigned> frostpunkPipelineCacheIoFailure;
#define PC_IO_FAIL(stage) (frostpunkPipelineCacheIoFailure.load() == (stage))
#else
#define PC_IO_FAIL(stage) false
#endif

#if defined(__ANDROID__) || defined(FP_PIPELINE_CACHE_TEST)
#include <fcntl.h>
#include <sys/stat.h>
#if defined(_WIN32)
// Test-only Win32 adapter; Android always executes the POSIX code below.
#include "pipeline_cache_io_win32.h"
#else
#include <unistd.h>
static intptr_t pcOpenDirectory(const char* path) {
  if (!path || path[0] != '/') return -1;
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat st;
  if (fd >= 0 && (!fstat(fd, &st) && S_ISDIR(st.st_mode)
      && st.st_uid == geteuid() && !(st.st_mode & 0077))) return fd;
  if (fd >= 0) close(fd);
  return -1;
}
static intptr_t pcOpenRead(intptr_t dir, const char* name) {
  return openat(int(dir), name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
}
static intptr_t pcOpenNew(intptr_t dir, const char* name) {
  return openat(int(dir), name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
}
static bool pcSize(intptr_t fd, size_t* bytes) {
  struct stat st;
  if (fstat(int(fd), &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid()
      || (st.st_mode & 0077) || st.st_size < 0 || uint64_t(st.st_size) > SIZE_MAX) return false;
  *bytes = size_t(st.st_size); return true;
}
static intptr_t pcRead(intptr_t fd, void* data, size_t bytes) { return read(int(fd), data, bytes); }
static intptr_t pcWrite(intptr_t fd, const void* data, size_t bytes) { return write(int(fd), data, bytes); }
static bool pcSync(intptr_t fd) { return !fsync(int(fd)); }
static bool pcClose(intptr_t fd) { return !close(int(fd)); }
static bool pcRename(intptr_t dir, const char* from, const char* to) {
  return !renameat(int(dir), from, int(dir), to);
}
static void pcRemove(intptr_t dir, const char* name) { unlinkat(int(dir), name, 0); }
static unsigned pcPid() { return unsigned(getpid()); }
#endif
#endif

namespace dxvk {
namespace {
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }
void put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = uint8_t(v >> (8 * i)); }
uint64_t checksum(const uint8_t* p, size_t bytes) {
  uint64_t v = 14695981039346656037ull;
  for (size_t i = 0; i < bytes; i++) v = (v ^ p[i]) * 1099511628211ull;
  return v;
}
}

DxvkPipelineCache::DxvkPipelineCache(VkDevice device, PFN_vkCreatePipelineCache create,
    PFN_vkDestroyPipelineCache destroy, PFN_vkGetPipelineCacheData getData,
    PFN_vkCreateGraphicsPipelines graphics, PFN_vkCreateComputePipelines compute,
    const VkPhysicalDeviceProperties& properties, const std::string& directory, bool enabled)
: m_device(device), m_create(create), m_destroy(destroy), m_getData(getData),
  m_graphics(graphics), m_compute(compute), m_properties(properties) {
#if defined(__ANDROID__) || defined(FP_PIPELINE_CACHE_TEST)
  int saved = errno;
  try {
    if (enabled && properties.vendorID == 0x13b5 && m_create && m_destroy && m_getData) {
      // The directory is the application's already-created, private profile.
      // Keep a directory fd, so replacement of the pathname cannot redirect I/O.
      m_directory = pcOpenDirectory(directory.c_str());
      if (m_directory >= 0) {
        char key[100];
        std::snprintf(key, sizeof(key), "fp-native-%08x-%08x-%08x-", properties.vendorID,
          properties.deviceID, properties.driverVersion);
        m_name = key;
        const char* hex = "0123456789abcdef";
        for (uint8_t value : properties.pipelineCacheUUID) {
          m_name += hex[value >> 4]; m_name += hex[value & 15];
        }
        m_name += '-'; m_name += FrostpunkPipelineCacheIdentityHex; m_name += ".bin";
        std::vector<uint8_t> blob;
        intptr_t fd = pcOpenRead(m_directory, m_name.c_str());
        if (fd >= 0) {
          try {
          size_t bytes = 0;
          if (pcSize(fd, &bytes) && bytes >= HeaderBytes + 32 && bytes <= HeaderBytes + MaxBytes) {
            blob.resize(bytes);
            size_t done = 0;
            while (done < bytes) {
              intptr_t n = pcRead(fd, blob.data() + done, bytes - done);
              if (n < 0 && errno == EINTR) continue;
              if (n <= 0) break;
              done += size_t(n);
            }
            uint8_t extra;
            if (done != bytes || pcRead(fd, &extra, 1) != 0) blob.clear();
          }
          } catch (...) { pcClose(fd); throw; }
          pcClose(fd);
        }
        VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        if (valid(blob)) { info.initialDataSize = blob.size() - HeaderBytes; info.pInitialData = blob.data() + HeaderBytes; }
        VkPipelineCache result = VK_NULL_HANDLE;
        size_t loadedBytes = info.initialDataSize;
        VkResult status = m_create(m_device, &info, nullptr, &result);
        bool initialAccepted = loadedBytes && status == VK_SUCCESS;
        // Vulkan does not define the output handle on create failure.
        if (status != VK_SUCCESS && info.initialDataSize) {
          info.initialDataSize = 0; info.pInitialData = nullptr; result = VK_NULL_HANDLE;
          status = m_create(m_device, &info, nullptr, &result);
        }
        if (status == VK_SUCCESS) m_cache = result;
        if (m_cache) {
          try { m_writer = std::thread([this] { run(); }); }
          catch (...) { m_destroy(m_device, m_cache, nullptr); m_cache = VK_NULL_HANDLE; }
        }
        std::fprintf(stderr, "NATIVE_PIPELINE_CACHE load_bytes=%zu initial_accepted=%u enabled=%u key=%s\n",
          loadedBytes, unsigned(initialAccepted), unsigned(m_cache != VK_NULL_HANDLE), m_name.c_str());
      }
    }
  } catch (...) {
    if (m_cache) m_destroy(m_device, m_cache, nullptr);
    m_cache = VK_NULL_HANDLE;
  }
  errno = saved;
#else
  (void)directory; (void)enabled;
#endif
}

DxvkPipelineCache::~DxvkPipelineCache() {
#if defined(__ANDROID__) || defined(FP_PIPELINE_CACHE_TEST)
  int saved = errno;
  // The owning device has stopped all pipeline workers, and its objects have
  // been destroyed before this member. No vkCreate* may still access the cache.
  { std::lock_guard<std::mutex> lock(m_mutex); m_stop = true; }
  m_changed.notify_one();
  if (m_writer.joinable()) m_writer.join();
  if (m_cache) m_destroy(m_device, m_cache, nullptr);
  if (m_directory >= 0) pcClose(m_directory);
  errno = saved;
#endif
}

VkResult DxvkPipelineCache::createGraphicsPipelines(uint32_t count, const VkGraphicsPipelineCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines) const {
  VkResult result = m_graphics(m_device, m_cache, count, info, allocator, pipelines);
  dirty();
  return result;
}
VkResult DxvkPipelineCache::createComputePipelines(uint32_t count, const VkComputePipelineCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines) const {
  VkResult result = m_compute(m_device, m_cache, count, info, allocator, pipelines);
  dirty();
  return result;
}
void DxvkPipelineCache::dirty() const {
  if (!m_cache) return;
  int saved = errno;
  { std::lock_guard<std::mutex> lock(m_mutex);
    if (m_generation != UINT64_MAX) ++m_generation;
  }
  m_changed.notify_one();
  errno = saved;
}

bool DxvkPipelineCache::valid(const std::vector<uint8_t>& b) const {
  if (b.size() < HeaderBytes + 32 || b.size() > HeaderBytes + MaxBytes) return false;
  const auto* p = b.data();
  if (std::memcmp(p, "FPVKPC02", 8) || get32(p + 8) != 2 || get32(p + 12) != HeaderBytes
      || get32(p + 16) != m_properties.vendorID || get32(p + 20) != m_properties.deviceID
      || get32(p + 24) != m_properties.driverVersion || get32(p + 28) != b.size() - HeaderBytes
      || std::memcmp(p + 32, m_properties.pipelineCacheUUID, VK_UUID_SIZE)
      || std::memcmp(p + 48, FrostpunkPipelineCacheIdentity, 32)
      || get64(p + 80) != checksum(p + HeaderBytes, b.size() - HeaderBytes)) return false;
  p += HeaderBytes;
  return get32(p) == 32 && get32(p + 4) == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
      && get32(p + 8) == m_properties.vendorID && get32(p + 12) == m_properties.deviceID
      && !std::memcmp(p + 16, m_properties.pipelineCacheUUID, VK_UUID_SIZE);
}
std::vector<uint8_t> DxvkPipelineCache::envelope(const std::vector<uint8_t>& data) const {
  std::vector<uint8_t> b(HeaderBytes + data.size(), 0);
  auto* p = b.data(); std::memcpy(p, "FPVKPC02", 8);
  put32(p + 8, 2); put32(p + 12, HeaderBytes); put32(p + 16, m_properties.vendorID);
  put32(p + 20, m_properties.deviceID); put32(p + 24, m_properties.driverVersion); put32(p + 28, uint32_t(data.size()));
  std::memcpy(p + 32, m_properties.pipelineCacheUUID, VK_UUID_SIZE);
  std::memcpy(p + 48, FrostpunkPipelineCacheIdentity, 32);
  put64(p + 80, checksum(data.data(), data.size()));
  std::copy(data.begin(), data.end(), b.begin() + HeaderBytes);
  return b;
}
bool DxvkPipelineCache::save() {
#if defined(__ANDROID__) || defined(FP_PIPELINE_CACHE_TEST)
  // Cache growth may race the size query. Only a complete VK_SUCCESS snapshot
  // is published; a failed/bounded-out snapshot leaves the previous file intact.
  std::vector<uint8_t> blob;
  for (unsigned attempt = 0; attempt < 3; attempt++) {
    size_t bytes = 0;
    if (m_getData(m_device, m_cache, &bytes, nullptr) != VK_SUCCESS || bytes < 32 || bytes > MaxBytes) return false;
    std::vector<uint8_t> data(bytes);
    VkResult result = m_getData(m_device, m_cache, &bytes, data.data());
    if (result == VK_INCOMPLETE) continue;
    if (result != VK_SUCCESS || bytes < 32 || bytes > data.size()) return false;
    data.resize(bytes); blob = envelope(data);
    if (!valid(blob)) return false;
    break;
  }
  if (blob.empty()) return false;
  char suffix[80];
  std::snprintf(suffix, sizeof(suffix), ".tmp-%u-%llx-%llx", pcPid(),
    (unsigned long long)uintptr_t(this),
    (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count());
  std::string temporary = m_name + suffix;
  intptr_t fd = PC_IO_FAIL(1) ? -1 : pcOpenNew(m_directory, temporary.c_str());
  if (fd < 0) return false;
  bool good = true; size_t done = 0;
  while (done < blob.size()) {
    intptr_t n = PC_IO_FAIL(2) ? -1 : pcWrite(fd, blob.data() + done, blob.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { good = false; break; }
    done += size_t(n);
  }
  if (good) good = !PC_IO_FAIL(3) && pcSync(fd);
  if (!pcClose(fd)) good = false;
  if (PC_IO_FAIL(4)) good = false;
  if (good) good = !PC_IO_FAIL(5) && pcRename(m_directory, temporary.c_str(), m_name.c_str());
  if (!good) pcRemove(m_directory, temporary.c_str());
  else pcSync(m_directory); // Rename is already atomic; directory durability is best effort.
  return good;
#else
  return false;
#endif
}
void DxvkPipelineCache::run() {
  std::unique_lock<std::mutex> lock(m_mutex);
  auto nextAttempt = std::chrono::steady_clock::time_point::min();
  while (true) {
    m_changed.wait(lock, [this] { return m_stop || m_generation != m_savedGeneration; });
    if (m_stop && m_generation == m_savedGeneration) break;
    if (!m_stop && std::chrono::steady_clock::now() < nextAttempt) {
      m_changed.wait_until(lock, nextAttempt, [this] { return m_stop; });
    }
    bool stopping = m_stop;
    uint64_t snapshotGeneration = m_generation; // Never acknowledge later creates.
    lock.unlock();
    bool good = false;
    try { good = save(); } catch (...) { /* Optional cache: keep the old valid file. */ }
    lock.lock();
    if (good) m_savedGeneration = snapshotGeneration;
    nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (stopping) break; // One bounded final attempt; failed I/O must not hang teardown.
  }
}
}
