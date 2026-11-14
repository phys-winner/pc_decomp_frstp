#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../native/windows/loader.h"

static void check_nx(const fp_image *image) {
    assert(fp_image_code(image, image->entry));
    assert(!fp_image_contains(image, (uint64_t)(uintptr_t)image->base - 1));
    assert(!fp_image_contains(image, (uint64_t)(uintptr_t)image->base + image->size));
    for (size_t offset = 0; offset < image->size;) {
        MEMORY_BASIC_INFORMATION info;
        assert(VirtualQuery((char *)image->base + offset, &info, sizeof(info)) == sizeof(info));
        assert(info.State == MEM_COMMIT);
        assert(!(info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)));
        size_t next = (size_t)((char *)info.BaseAddress + info.RegionSize - (char *)image->base);
        assert(next > offset);
        offset = next;
    }
}

int wmain(int argc, wchar_t **argv) {
    assert(argc == 3);
    wchar_t directory[4096];
    assert(wcslen(argv[2]) < 4096);
    wcscpy_s(directory, 4096, argv[2]);
    wchar_t *last = wcsrchr(directory, L'\\');
    if (!last) last = wcsrchr(directory, L'/');
    assert(last);
    *last = 0;
    assert(SetDllDirectoryW(directory));
    void *reservation = NULL;
    if (!wcscmp(argv[1], L"reject-reloc")) {
        reservation = VirtualAlloc((void *)(uintptr_t)UINT64_C(0x140000000), 0x3000,
            MEM_RESERVE, PAGE_NOACCESS);
        assert(reservation == (void *)(uintptr_t)UINT64_C(0x140000000));
    }
    fp_image image;
    int loaded = fp_load_image(argv[2], &image);
    if (!wcscmp(argv[1], L"reject") || !wcscmp(argv[1], L"reject-reloc")) {
        assert(!loaded && !image.base);
    } else {
        assert(loaded);
        check_nx(&image);
        if (!wcscmp(argv[1], L"synthetic")) {
            uint64_t value;
            memcpy(&value, (char *)image.base + 0x1000, 8);
            assert(value == (uint64_t)(uintptr_t)image.base + 0x123);
            fp_image second;
            assert(fp_load_image(argv[2], &second));
            assert(second.base != image.base);
            check_nx(&second);
            memcpy(&value, (char *)second.base + 0x1000, 8);
            assert(value == (uint64_t)(uintptr_t)second.base + 0x123);
            fp_unload_image(&second);
        }
        fp_unload_image(&image);
        assert(!image.base);
    }
    if (reservation) assert(VirtualFree(reservation, 0, MEM_RELEASE));
    puts("WINDOWS_LOADER_PASS original_bytes_executed=0");
    return 0;
}
