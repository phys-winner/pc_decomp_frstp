#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct {
    uint32_t rva, size, characteristics;
} fp_section;

typedef struct {
    void *base;
    size_t size;
    uint64_t preferred_base, entry, tls_callbacks, tls_start, tls_end, tls_index;
    uint32_t tls_zero;
    fp_section *sections;
    size_t section_count;
    void **modules;
    size_t module_count;
} fp_image;

/* Maps data only. Original image instructions are never executable. */
int fp_load_image(const wchar_t *path, fp_image *image);
void fp_unload_image(fp_image *image);
int fp_image_contains(const fp_image *image, uint64_t address);
int fp_image_code(const fp_image *image, uint64_t address);
