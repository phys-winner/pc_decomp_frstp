#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"

static int span(size_t total, uint64_t offset, uint64_t bytes) {
    return offset <= total && bytes <= total - offset;
}

int fp_image_contains(const fp_image *image, uint64_t address) {
    uint64_t base = (uint64_t)(uintptr_t)image->base;
    return image->base && address >= base && address - base < image->size;
}

int fp_image_code(const fp_image *image, uint64_t address) {
    if (!fp_image_contains(image, address)) return 0;
    uint64_t offset = address - (uint64_t)(uintptr_t)image->base;
    for (size_t i = 0; i < image->section_count; ++i) {
        const fp_section *section = &image->sections[i];
        if ((section->characteristics & IMAGE_SCN_MEM_EXECUTE) &&
            offset >= section->rva && offset - section->rva < section->size) return 1;
    }
    return 0;
}

void fp_unload_image(fp_image *image) {
    for (size_t i = image->module_count; i; --i) FreeLibrary((HMODULE)image->modules[i - 1]);
    if (image->base) VirtualFree(image->base, 0, MEM_RELEASE);
    free(image->sections);
    free(image->modules);
    memset(image, 0, sizeof(*image));
}

static const char *image_string(const fp_image *image, uint64_t offset) {
    if (!span(image->size, offset, 1)) return NULL;
    const char *text = (const char *)image->base + offset;
    return memchr(text, 0, image->size - (size_t)offset) ? text : NULL;
}

static int relocate(fp_image *image, IMAGE_DATA_DIRECTORY directory) {
    uint64_t delta = (uint64_t)(uintptr_t)image->base - image->preferred_base;
    if (!delta) return 1;
    if (!directory.Size) {
        fprintf(stderr, "PE_STOP preferred address unavailable and no base relocations\n");
        return 0;
    }
    unsigned char *base = image->base;
    uint64_t cursor = directory.VirtualAddress, end = cursor + directory.Size;
    while (cursor < end) {
        if (!span((size_t)end, cursor, sizeof(IMAGE_BASE_RELOCATION))) return 0;
        IMAGE_BASE_RELOCATION block;
        memcpy(&block, base + cursor, sizeof(block));
        if (block.SizeOfBlock < sizeof(block) || (block.SizeOfBlock - sizeof(block)) % 2 ||
            !span((size_t)end, cursor, block.SizeOfBlock)) return 0;
        size_t count = (block.SizeOfBlock - sizeof(block)) / 2;
        for (size_t i = 0; i < count; ++i) {
            WORD item;
            memcpy(&item, base + cursor + sizeof(block) + i * 2, 2);
            unsigned type = item >> 12;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            uint64_t offset = (uint64_t)block.VirtualAddress + (item & 4095);
            if (type != IMAGE_REL_BASED_DIR64 || !span(image->size, offset, 8)) return 0;
            uint64_t pointer;
            memcpy(&pointer, base + offset, 8);
            pointer += delta;
            memcpy(base + offset, &pointer, 8);
        }
        cursor += block.SizeOfBlock;
    }
    return 1;
}

static int bind_imports(fp_image *image, IMAGE_DATA_DIRECTORY directory) {
    if (!directory.Size) return 1;
    size_t capacity = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    image->modules = calloc(capacity, sizeof(*image->modules));
    if (!image->modules) return 0;
    unsigned char *base = image->base;
    for (size_t n = 0; n < capacity; ++n) {
        IMAGE_IMPORT_DESCRIPTOR descriptor;
        memcpy(&descriptor, base + directory.VirtualAddress + n * sizeof(descriptor), sizeof(descriptor));
        if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk &&
            !descriptor.TimeDateStamp && !descriptor.ForwarderChain) return 1;
        const char *dll = image_string(image, descriptor.Name);
        if (!descriptor.Name || !descriptor.FirstThunk || !dll || !*dll) return 0;
        wchar_t wide[MAX_PATH];
        size_t length = strlen(dll);
        if (length >= MAX_PATH) return 0;
        for (size_t i = 0; i <= length; ++i) {
            unsigned char ch = (unsigned char)dll[i];
            if (ch > 127 || ch == '/' || ch == '\\' || ch == ':') return 0;
            wide[i] = ch;
        }
        HMODULE module = LoadLibraryW(wide);
        if (!module) {
            fprintf(stderr, "PE_STOP LoadLibrary %s error=%lu\n", dll, GetLastError());
            return 0;
        }
        image->modules[image->module_count++] = module;
        uint64_t source = descriptor.OriginalFirstThunk ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
        uint64_t destination = descriptor.FirstThunk;
        for (;;) {
            if (!span(image->size, source, 8) || !span(image->size, destination, 8)) return 0;
            uint64_t value;
            memcpy(&value, base + source, 8);
            if (!value) break;
            const char *name;
            char ordinal_name[32];
            if (value & IMAGE_ORDINAL_FLAG64) {
                if (value & ~(IMAGE_ORDINAL_FLAG64 | UINT64_C(65535))) return 0;
                snprintf(ordinal_name, sizeof(ordinal_name), "#%u", (unsigned)(value & 65535));
                name = ordinal_name;
            } else {
                if (value > UINT32_MAX || !span(image->size, value, 3)) return 0;
                name = image_string(image, value + 2);
                if (!name || !*name) return 0;
            }
            FARPROC procedure = GetProcAddress(module, (value & IMAGE_ORDINAL_FLAG64) ?
                MAKEINTRESOURCEA((WORD)value) : name);
            if (!procedure) {
                fprintf(stderr, "PE_STOP unresolved import %s!%s error=%lu\n", dll, name, GetLastError());
                return 0;
            }
            uint64_t pointer = (uint64_t)(uintptr_t)procedure;
            memcpy(base + destination, &pointer, 8);
            source += 8;
            destination += 8;
        }
    }
    return 0;
}

static int load_tls(fp_image *image, IMAGE_DATA_DIRECTORY directory) {
    if (!directory.Size) return 1;
    if (directory.Size < sizeof(IMAGE_TLS_DIRECTORY64)) return 0;
    IMAGE_TLS_DIRECTORY64 tls;
    memcpy(&tls, (unsigned char *)image->base + directory.VirtualAddress, sizeof(tls));
    uint64_t base = (uint64_t)(uintptr_t)image->base;
    if (tls.EndAddressOfRawData < tls.StartAddressOfRawData ||
        (tls.StartAddressOfRawData && (tls.StartAddressOfRawData < base ||
        !span(image->size, tls.StartAddressOfRawData - base,
            tls.EndAddressOfRawData - tls.StartAddressOfRawData))) ||
        (!tls.StartAddressOfRawData && tls.EndAddressOfRawData) ||
        !tls.AddressOfIndex || tls.AddressOfIndex < base ||
        !span(image->size, tls.AddressOfIndex - base, 4)) return 0;
    if (tls.AddressOfCallBacks) {
        uint64_t cursor = tls.AddressOfCallBacks;
        for (;;) {
            if (cursor < base || !span(image->size, cursor - base, 8)) return 0;
            uint64_t callback;
            memcpy(&callback, (void *)(uintptr_t)cursor, 8);
            if (!callback) break;
            if (!fp_image_code(image, callback)) return 0;
            cursor += 8;
        }
    }
    image->tls_start = tls.StartAddressOfRawData;
    image->tls_end = tls.EndAddressOfRawData;
    image->tls_index = tls.AddressOfIndex;
    image->tls_callbacks = tls.AddressOfCallBacks;
    image->tls_zero = tls.SizeOfZeroFill;
    return 1;
}

int fp_load_image(const wchar_t *path, fp_image *image) {
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned char *bytes = NULL;
    const char *stage = "read input";
    int ok = 0;
    memset(image, 0, sizeof(*image));
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER length;
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &length) ||
        length.QuadPart < sizeof(IMAGE_DOS_HEADER) || length.QuadPart > UINT32_MAX) goto done;
    size_t size = (size_t)length.QuadPart;
    bytes = malloc(size);
    if (!bytes) goto done;
    for (size_t cursor = 0; cursor < size;) {
        DWORD got, chunk = (DWORD)((size - cursor) > 1048576 ? 1048576 : size - cursor);
        if (!ReadFile(file, bytes + cursor, chunk, &got, NULL) || got != chunk) goto done;
        cursor += got;
    }
    stage = "PE headers";
    IMAGE_DOS_HEADER dos;
    memcpy(&dos, bytes, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < (LONG)sizeof(dos) ||
        !span(size, (uint32_t)dos.e_lfanew, sizeof(IMAGE_NT_HEADERS64))) goto done;
    IMAGE_NT_HEADERS64 nt;
    memcpy(&nt, bytes + dos.e_lfanew, sizeof(nt));
    IMAGE_OPTIONAL_HEADER64 *optional = &nt.OptionalHeader;
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        optional->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(*optional) ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96 ||
        !optional->SizeOfImage || optional->SizeOfImage > INT32_MAX ||
        !optional->SizeOfHeaders || optional->SizeOfHeaders > optional->SizeOfImage ||
        !span(size, 0, optional->SizeOfHeaders) || optional->NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        !optional->ImageBase || optional->ImageBase > UINT64_MAX - optional->SizeOfImage ||
        optional->ImageBase % 65536) goto done;
    uint64_t sections_offset = (uint32_t)dos.e_lfanew + (uint64_t)offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    if (!span(optional->SizeOfHeaders, sections_offset,
        (uint64_t)nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER))) goto done;
    for (unsigned i = 0; i < optional->NumberOfRvaAndSizes; ++i) {
        IMAGE_DATA_DIRECTORY d = optional->DataDirectory[i];
        if (d.Size && !d.VirtualAddress) goto done;
        if (i == IMAGE_DIRECTORY_ENTRY_SECURITY) {
            if (d.Size && !span(size, d.VirtualAddress, d.Size)) goto done;
        } else if (d.Size && !span(optional->SizeOfImage, d.VirtualAddress, d.Size)) goto done;
    }
    image->section_count = nt.FileHeader.NumberOfSections;
    image->sections = calloc(image->section_count, sizeof(*image->sections));
    if (!image->sections) goto done;
    stage = "PE sections";
    for (size_t i = 0; i < image->section_count; ++i) {
        IMAGE_SECTION_HEADER section;
        memcpy(&section, bytes + sections_offset + i * sizeof(section), sizeof(section));
        uint32_t extent = section.Misc.VirtualSize > section.SizeOfRawData ?
            section.Misc.VirtualSize : section.SizeOfRawData;
        if (!span(optional->SizeOfImage, section.VirtualAddress, extent) ||
            (extent && section.VirtualAddress < optional->SizeOfHeaders) ||
            (section.SizeOfRawData && (!section.PointerToRawData ||
            !span(size, section.PointerToRawData, section.SizeOfRawData)))) goto done;
        image->sections[i] = (fp_section){section.VirtualAddress, extent, section.Characteristics};
        for (size_t j = 0; j < i; ++j) {
            fp_section other = image->sections[j];
            if (extent && other.size && section.VirtualAddress < (uint64_t)other.rva + other.size &&
                other.rva < (uint64_t)section.VirtualAddress + extent) goto done;
        }
    }
    stage = "allocate NX image";
    image->size = optional->SizeOfImage;
    image->preferred_base = optional->ImageBase;
    image->base = VirtualAlloc((void *)(uintptr_t)optional->ImageBase, image->size,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image->base) image->base = VirtualAlloc(NULL, image->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image->base) goto done;
    memcpy(image->base, bytes, optional->SizeOfHeaders);
    for (size_t i = 0; i < image->section_count; ++i) {
        IMAGE_SECTION_HEADER section;
        memcpy(&section, bytes + sections_offset + i * sizeof(section), sizeof(section));
        if (section.SizeOfRawData) memcpy((unsigned char *)image->base + section.VirtualAddress,
            bytes + section.PointerToRawData, section.SizeOfRawData);
    }
    image->entry = (uint64_t)(uintptr_t)image->base + optional->AddressOfEntryPoint;
    if (!fp_image_code(image, image->entry)) goto done;
    stage = "base relocations";
    IMAGE_DATA_DIRECTORY empty = {0};
#define DIRECTORY(i) (optional->NumberOfRvaAndSizes > (i) ? optional->DataDirectory[i] : empty)
    if (!relocate(image, DIRECTORY(IMAGE_DIRECTORY_ENTRY_BASERELOC))) goto done;
    stage = "native imports";
    if (!bind_imports(image, DIRECTORY(IMAGE_DIRECTORY_ENTRY_IMPORT))) goto done;
    stage = "TLS metadata";
    if (!load_tls(image, DIRECTORY(IMAGE_DIRECTORY_ENTRY_TLS))) goto done;
#undef DIRECTORY
    fprintf(stderr, "PE_READY base=0x%" PRIx64 " preferred=0x%" PRIx64 " size=%zu entry=0x%" PRIx64
        " original_code_NX=1 original_bytes_executed=0\n", (uint64_t)(uintptr_t)image->base,
        image->preferred_base, image->size, image->entry);
    ok = 1;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(bytes);
    if (!ok) {
        fprintf(stderr, "PE_STOP stage=%s error=%lu\n", stage, GetLastError());
        fp_unload_image(image);
    }
    return ok;
}
