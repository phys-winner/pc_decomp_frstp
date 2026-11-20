#include "cpu64.h"

#include <stdlib.h>
#include <string.h>
#include <uchar.h>

enum { BUILTIN_LANGUAGES = 16, FAN_LANGUAGES = 2 };

// LiquidEngine's language descriptor has a GUID, an interned name and a UTF-16 caption.
static struct Language {
    unsigned char guid[16];
    uint64_t name;
    const char *code;
    const char16_t *caption;
    uint64_t flags;
} languages[FAN_LANGUAGES];

static uint64_t registry[BUILTIN_LANGUAGES + FAN_LANGUAGES];
static unsigned count = BUILTIN_LANGUAGES;

unsigned fp_fan_language_count(void) { return count; }

uint64_t fp_fan_language_registry(CPU *cpu, uint64_t original) {
    if (count > BUILTIN_LANGUAGES) return (uintptr_t)registry;
    const char *enabled = getenv("FROSTPUNK_FAN_LANGUAGES");
    if (!enabled || !*enabled || !rd64(rd64(original + 8) + 0x10)) return original;

    static const struct {
        const char *code, *name, *guid;
        const char16_t *caption;
    } entries[FAN_LANGUAGES] = {
        {"cs", "Czech", "00000000-0000-0000-0000-000007800010", u"Čeština"},
        {"hu", "Hungarian", "00000000-0000-0000-0000-000007800011", u"Magyar"},
    };
    memcpy(registry, (const void *)(uintptr_t)original, BUILTIN_LANGUAGES * sizeof(*registry));
    unsigned added = 0;
    for (unsigned i = 0; i < FAN_LANGUAGES; ++i) {
        if (!strstr(enabled, entries[i].code)) continue;
        struct Language *language = &languages[added];
        language->code = entries[i].code;
        language->caption = entries[i].caption;

        CPU call = *cpu;
        uint64_t stack = (cpu->rsp - 128) & ~UINT64_C(15);
        call.rsp = stack;
        call.rcx = (uintptr_t)language->guid;
        call.rdx = (uintptr_t)entries[i].guid;
        push64(&call, UINT64_C(0xe5ee0010));
        dispatch(&call, UINT64_C(0x140E1E6F0));
        call.rsp = stack;
        call.rcx = (uintptr_t)&language->name;
        call.rdx = (uintptr_t)entries[i].name;
        push64(&call, UINT64_C(0xe5ee0010));
        dispatch(&call, UINT64_C(0x140E538C0));
        registry[BUILTIN_LANGUAGES + added++] = (uintptr_t)language;
    }
    count += added;
    return added ? (uintptr_t)registry : original;
}

int fp_fan_language_enabled(uint64_t descriptor) {
    for (unsigned i = BUILTIN_LANGUAGES; i < count; ++i)
        if (registry[i] == descriptor) return 1;
    return 0;
}
