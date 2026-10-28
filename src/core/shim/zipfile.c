#include "zipfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>

#define SIG_EOCD   0x06054b50u
#define SIG_CDIR   0x02014b50u
#define SIG_LOCAL  0x04034b50u
#define EOCD_MIN   22
#define EOCD_SCAN  (EOCD_MIN + 0xFFFF) // including max comment length

typedef struct {
	char name[128];
	char path[160]; // full name in the zip (folders included)
	uint32_t crc;
	uint32_t csize;
	uint32_t usize;
	uint32_t local_ofs;
	uint16_t method;
} zip_entry;

struct rei_zip {
	FILE *f;
	int count;
	zip_entry *entries;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int read_at(FILE *f, long ofs, void *buf, size_t len)
{
	if (fseek(f, ofs, SEEK_SET) != 0)
		return -1;
	return fread(buf, 1, len, f) == len ? 0 : -1;
}

rei_zip *rei_zip_open(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;

	rei_zip *z = (rei_zip *)calloc(1, sizeof(*z));
	uint8_t *tail = NULL, *cdir = NULL;
	if (!z)
		goto fail;
	z->f = f;

	if (fseek(f, 0, SEEK_END) != 0)
		goto fail;
	long fsize = ftell(f);
	if (fsize < EOCD_MIN)
		goto fail;

	// search for the EOCD signature from the end
	long scan = fsize < EOCD_SCAN ? fsize : EOCD_SCAN;
	tail = (uint8_t *)malloc(scan);
	if (!tail || read_at(f, fsize - scan, tail, scan) != 0)
		goto fail;
	long eocd = -1;
	for (long i = scan - EOCD_MIN; i >= 0; i--) {
		if (rd32(tail + i) == SIG_EOCD) {
			eocd = i;
			break;
		}
	}
	if (eocd < 0)
		goto fail;

	uint16_t count = rd16(tail + eocd + 10);
	uint32_t cd_size = rd32(tail + eocd + 12);
	uint32_t cd_ofs = rd32(tail + eocd + 16);
	if (cd_ofs == 0xFFFFFFFFu || (long)cd_ofs + (long)cd_size > fsize)
		goto fail; // ZIP64 unsupported

	cdir = (uint8_t *)malloc(cd_size ? cd_size : 1);
	z->entries = (zip_entry *)calloc(count ? count : 1, sizeof(zip_entry));
	if (!cdir || !z->entries || read_at(f, cd_ofs, cdir, cd_size) != 0)
		goto fail;

	uint32_t p = 0;
	for (int i = 0; i < count; i++) {
		if (p + 46 > cd_size || rd32(cdir + p) != SIG_CDIR)
			goto fail;
		const uint8_t *h = cdir + p;
		uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
		if (p + 46 + nlen > cd_size)
			goto fail;

		zip_entry *e = &z->entries[z->count];
		e->method = rd16(h + 10);
		e->crc = rd32(h + 16);
		e->csize = rd32(h + 20);
		e->usize = rd32(h + 24);
		e->local_ofs = rd32(h + 42);

		// drop directory path, keep only the file name
		const char *nm = (const char *)(h + 46);
		int start = 0;
		for (int k = 0; k < nlen; k++)
			if (nm[k] == '/' || nm[k] == '\\')
				start = k + 1;
		int n = nlen - start;
		{
			const int pn = nlen < (int)sizeof(e->path) ? nlen : (int)sizeof(e->path) - 1;
			memcpy(e->path, nm, pn);
			e->path[pn] = 0;
		}
		if (n > 0) {
			if (n >= (int)sizeof(e->name))
				n = sizeof(e->name) - 1;
			memcpy(e->name, nm + start, n);
			e->name[n] = '\0';
			z->count++;
		}
		p += 46 + nlen + xlen + clen;
	}

	free(tail);
	free(cdir);
	return z;

fail:
	free(tail);
	free(cdir);
	if (z) {
		free(z->entries);
		free(z);
	}
	fclose(f);
	return NULL;
}

void rei_zip_close(rei_zip *z)
{
	if (!z)
		return;
	fclose(z->f);
	free(z->entries);
	free(z);
}

int rei_zip_find(const rei_zip *z, const char *name, uint32_t crc)
{
	if (name) {
		for (int i = 0; i < z->count; i++)
			if (strcasecmp(z->entries[i].name, name) == 0)
				return i;
	}
	if (crc) {
		for (int i = 0; i < z->count; i++)
			if (z->entries[i].crc == crc)
				return i;
	}
	return -1;
}

uint32_t rei_zip_entry_size(const rei_zip *z, int idx) { return z->entries[idx].usize; }
int rei_zip_count(const rei_zip *z) { return z->count; }
const char *rei_zip_entry_path(const rei_zip *z, int idx) { return z->entries[idx].path; }
uint32_t rei_zip_entry_crc(const rei_zip *z, int idx) { return z->entries[idx].crc; }

void (*rei_zip_tick)(uint32_t done, uint32_t total);

int rei_zip_read(rei_zip *z, int idx, uint8_t *dst, uint32_t cap, uint32_t *out_len)
{
	const zip_entry *e = &z->entries[idx];
	uint8_t lh[30];
	if (read_at(z->f, e->local_ofs, lh, sizeof(lh)) != 0 || rd32(lh) != SIG_LOCAL)
		return -1;
	long data_ofs = (long)e->local_ofs + 30 + rd16(lh + 26) + rd16(lh + 28);

	uint8_t *src = (uint8_t *)malloc(e->csize ? e->csize : 1);
	if (!src)
		return -1;
	int ret = -1;
	if (read_at(z->f, data_ofs, src, e->csize) != 0)
		goto out;

	// if the destination buffer is small, write only the head. Inflate fully for the CRC check.
	uint8_t *full = dst;
	if (e->usize > cap) {
		full = (uint8_t *)malloc(e->usize);
		if (!full)
			goto out;
	}

	if (e->method == 0) {
		if (e->csize != e->usize)
			goto out_full;
		memcpy(full, src, e->usize);
	} else if (e->method == 8) {
		z_stream s;
		memset(&s, 0, sizeof(s));
		if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
			goto out_full;
		s.next_in = src;
		s.avail_in = e->csize;
		s.next_out = full;
		int r = Z_OK;
		if (!rei_zip_tick) {
			s.avail_out = e->usize;
			r = inflate(&s, Z_FINISH);
		} else { // (in steps, reporting the bytes so far: the loading screen's bar moves during a large ROM)
			const uint32_t step = 256 * 1024;
			while (r == Z_OK) {
				const uint32_t left = e->usize - (uint32_t)s.total_out;
				s.avail_out = left < step ? left : step;
				r = inflate(&s, left <= step ? Z_FINISH : Z_NO_FLUSH);
				rei_zip_tick((uint32_t)s.total_out, e->usize);
				if (r == Z_BUF_ERROR && s.avail_out == 0 && s.total_out < e->usize)
					r = Z_OK;
			}
		}
		inflateEnd(&s);
		if (r != Z_STREAM_END || s.total_out != e->usize)
			goto out_full;
	} else {
		goto out_full;
	}

	if ((uint32_t)crc32(0L, full, e->usize) != e->crc)
		goto out_full;

	if (full != dst)
		memcpy(dst, full, cap);
	if (out_len)
		*out_len = e->usize < cap ? e->usize : cap;
	ret = 0;

out_full:
	if (full != dst)
		free(full);
out:
	free(src);
	return ret;
}
