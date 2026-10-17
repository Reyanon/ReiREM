#ifndef REI_ZIPFILE_H
#define REI_ZIPFILE_H

// Minimal zlib-based zip reader (ADR-002: zip extraction option a)
// - Reads the central directory once to build the entry list.
// - Supports only stored (0) and deflate (8). No encryption or ZIP64.
// - Entries can also be found by CRC32 instead of ROM name (works with differently named romsets).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rei_zip rei_zip;

rei_zip *rei_zip_open(const char *path);
void rei_zip_close(rei_zip *z);

// Find an entry. Name (case-insensitive) first, then crc. Returns -1 on failure
int rei_zip_find(const rei_zip *z, const char *name, uint32_t crc);

uint32_t rei_zip_entry_size(const rei_zip *z, int idx);
uint32_t rei_zip_entry_crc(const rei_zip *z, int idx);

// Extract up to cap bytes into dst. Returns 0 on success, actual length in *out_len.
// Fails if the CRC32 after extraction differs from the central directory value.
int rei_zip_read(rei_zip *z, int idx, uint8_t *dst, uint32_t cap, uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
