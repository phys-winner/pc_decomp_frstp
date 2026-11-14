#ifndef FROSTPUNK_BSWAP_H
#define FROSTPUNK_BSWAP_H

static inline uint32_t frostpunk_bswap32(uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0x0000FF00u) |
           ((value << 8) & 0x00FF0000u) | (value << 24);
}
static inline uint64_t frostpunk_bswap64(uint64_t value) {
    return ((uint64_t)frostpunk_bswap32((uint32_t)value) << 32) |
           frostpunk_bswap32((uint32_t)(value >> 32));
}

#endif
