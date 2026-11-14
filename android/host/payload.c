#include "payload.h"
#include "win64.h"
#include "../../generated/payload/payload_expected.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t va; frostpunk_payload_fn fn; } payload_entry;
extern const payload_entry payload_recomp_dispatch_table[];
extern const uint32_t payload_recomp_dispatch_count;
typedef struct { uint64_t slot; const char *dll; const char *name; } payload_import;
extern const payload_import payload_recomp_imports[];
static atomic_int validated;

int frostpunk_payload_import(CPU *c, uint64_t target) {
    const uint64_t sentinel_base = UINT64_C(0xE5E60000);
    const uint32_t import_count = 92;
    if (target < sentinel_base || target - sentinel_base >= (uint64_t)import_count * 8) return 0;
    if ((target - sentinel_base) % 8) {
        frostpunk_unsupported(target, "unaligned payload import sentinel");
        return 0;
    }
    const payload_import *import = &payload_recomp_imports[(target - sentinel_base) / 8];
    if (win64_dispatch_import(c, import->dll, import->name)) return 1;
    fprintf(stderr, "STOP unimplemented payload import %s!%s slot=0x%" PRIx64 " target=0x%" PRIx64 "\n",
            import->dll, import->name, import->slot, target);
    exit(70);
}

static int readable_nx(uint64_t address, size_t bytes) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    uint64_t cursor = address, end = address + bytes;
    char line[1024], mode[5];
    while (fgets(line, sizeof(line), maps)) {
        uint64_t low, high;
        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s", &low, &high, mode) != 3) continue;
        if (low <= cursor && cursor < high) {
            if (mode[0] != 'r' || mode[2] == 'x') break;
            cursor = high < end ? high : end;
            if (cursor == end) { fclose(maps); return 1; }
        }
    }
    fclose(maps);
    return 0;
}

frostpunk_payload_fn frostpunk_payload_lookup(uint64_t target) {
    if (target < FROSTPUNK_PAYLOAD_BASE || target - FROSTPUNK_PAYLOAD_BASE >= FROSTPUNK_PAYLOAD_SIZE)
        return NULL;
    if (!atomic_load_explicit(&validated, memory_order_acquire)) {
        uint64_t code = FROSTPUNK_PAYLOAD_BASE + FROSTPUNK_PAYLOAD_TEXT_RVA;
        size_t bytes = sizeof(frostpunk_payload_expected_text);
        if (!readable_nx(code, bytes)) {
            frostpunk_unsupported(target, "payload original code is not mapped readable/NX");
            return NULL;
        }
        const void *mapped = win64_pointer(code, bytes, 0);
        if (memcmp(mapped, frostpunk_payload_expected_text, bytes)) {
            frostpunk_unsupported(target, "payload original .text bytes differ from hash-pinned static input");
            return NULL;
        }
        atomic_store_explicit(&validated, 1, memory_order_release);
        fprintf(stderr, "STATIC_PAYLOAD validated original_text_bytes=%zu file_sha256=%s original_bytes_executed=0\n",
                bytes, FROSTPUNK_PAYLOAD_SHA256);
    }
    uint32_t low = 0, high = payload_recomp_dispatch_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (payload_recomp_dispatch_table[mid].va < target) low = mid + 1; else high = mid;
    }
    if (low == payload_recomp_dispatch_count || payload_recomp_dispatch_table[low].va != target) {
        frostpunk_unsupported(target, "payload missing static dispatch target");
        return NULL;
    }
    return payload_recomp_dispatch_table[low].fn;
}
