// User cheat files (FBNeo cheat .ini format, docs/CHEATS.md). Shared by Vita and PSP, rei_core.h rei_core_cheat_*
//
//   // comment
//   cheat "Infinite Lives"           <- one cheat (name)
//   type 0                           <- 0 = write every frame while on, 1 = write once when selected
//   default 0                        <- initially selected option (usually 0 = off)
//   0 "Disabled"                     <- option 0: off when it has nothing to write
//   1 "Enabled", 0, 0x0E0A0D, 0x09   <- option 1: CPU index, address, value (repeat triples for multiple writes)
//
// FBNeo cheats/<set>.ini work as is. Values above 0xff are written as multiple little-endian bytes.
// CPU index follows FBNeo: the order the driver registered CPUs (0 = usually the main CPU). In modes where the sound CPU runs
// on another thread or the ME, only the main CPU (0) is written
#include "burnint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rei_core.h"

#define MAX_CPUS 8
#define MAX_CHEATS 128
#define MAX_OPTS 32
#define MAX_WRITES 512 // (address, value) pairs across all cheats

static struct { cpu_core_config *cfg; int n; } s_cpus[MAX_CPUS];
static int s_ncpus;

void CpuCheatRegister(INT32 type, cpu_core_config *config)
{
	if (s_ncpus < MAX_CPUS) {
		s_cpus[s_ncpus].cfg = config;
		s_cpus[s_ncpus].n = type;
		s_ncpus++;
	}
}

struct cheat_write { UINT8 cpu; UINT8 bytes; UINT32 addr, value; };
struct cheat_opt { char name[48]; int first, count; }; // range within s_writes
struct cheat { char name[64]; int type, sel, nopts, applied; cheat_opt opt[MAX_OPTS]; };

static cheat s_cheats[MAX_CHEATS];
static int s_ncheats;
static cheat_write s_writes[MAX_WRITES];
static int s_nwrites;
static int s_main_only; // sound CPU on another thread: main CPU (0) only

extern "C" void rei_cheats_reset(void) // before driver init (CPU registration starts fresh)
{
	s_ncpus = 0;
	s_ncheats = 0;
	s_nwrites = 0;
}

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

// Read "..." into out. Returns the position after the closing quote (NULL if none)
static const char *read_quoted(const char *p, char *out, int n)
{
	p = skip_ws(p);
	if (*p != '"')
		return NULL;
	p++;
	int k = 0;
	while (*p && *p != '"') {
		if (k < n - 1)
			out[k++] = *p;
		p++;
	}
	out[k] = 0;
	return *p == '"' ? p + 1 : NULL;
}

static int parse_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[512];
	cheat *cur = NULL;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		const char *p = skip_ws(line);
		if (!*p || (p[0] == '/' && p[1] == '/') || *p == ';' || *p == '#')
			continue;
		if (!strncmp(p, "cheat", 5) && (p[5] == ' ' || p[5] == '\t' || p[5] == '"')) {
			if (s_ncheats >= MAX_CHEATS)
				break;
			cur = &s_cheats[s_ncheats];
			memset(cur, 0, sizeof(*cur));
			if (!read_quoted(p + 5, cur->name, sizeof(cur->name))) {
				cur = NULL;
				continue;
			}
			s_ncheats++;
		} else if (!cur) {
			continue;
		} else if (!strncmp(p, "type", 4)) {
			cur->type = atoi(p + 4);
		} else if (!strncmp(p, "default", 7)) {
			cur->sel = atoi(p + 7);
		} else if (*p >= '0' && *p <= '9') { // option: index "name"[, cpu, address, value]...
			char *e;
			const long idx = strtol(p, &e, 10);
			if (idx != cur->nopts || cur->nopts >= MAX_OPTS)
				continue; // (indices run from 0 in order)
			cheat_opt *o = &cur->opt[cur->nopts];
			const char *q = read_quoted(e, o->name, sizeof(o->name));
			if (!q)
				continue;
			o->first = s_nwrites;
			o->count = 0;
			for (;;) {
				q = skip_ws(q);
				if (*q != ',')
					break;
				unsigned long v[3];
				int ok = 1;
				for (int k = 0; k < 3 && ok; k++) {
					q = skip_ws(q);
					if (*q != ',') { ok = 0; break; }
					q = skip_ws(q + 1);
					v[k] = strtoul(q, &e, 0);
					ok = e != q;
					q = e;
				}
				if (!ok || s_nwrites >= MAX_WRITES)
					break;
				cheat_write *w = &s_writes[s_nwrites++];
				w->cpu = (UINT8)v[0];
				w->addr = (UINT32)v[1];
				w->value = (UINT32)v[2];
				w->bytes = v[2] > 0xffffff ? 4 : v[2] > 0xffff ? 3 : v[2] > 0xff ? 2 : 1;
				o->count++;
			}
			cur->nopts++;
		}
	}
	fclose(f);
	for (int i = 0; i < s_ncheats; i++)
		if (s_cheats[i].sel < 0 || s_cheats[i].sel >= s_cheats[i].nopts)
			s_cheats[i].sel = 0;
	return s_ncheats;
}

// <set>.ini in dir (parent set .ini if missing). Returns cheats read (0 if none)
extern "C" int rei_cheats_load(const char *dir, const char *set, const char *parent, int main_only)
{
	s_ncheats = 0;
	s_nwrites = 0;
	s_main_only = main_only;
	char path[256];
	snprintf(path, sizeof(path), "%s%s.ini", dir, set);
	int n = parse_file(path);
	if (n <= 0 && parent && parent[0]) {
		s_ncheats = 0;
		s_nwrites = 0;
		snprintf(path, sizeof(path), "%s%s.ini", dir, parent);
		n = parse_file(path);
	}
	return n > 0 ? n : 0;
}

static void apply_opt(const cheat_opt *o)
{
	for (int k = 0; k < o->count; k++) {
		const cheat_write *w = &s_writes[o->first + k];
		if (w->cpu >= s_ncpus || (s_main_only && w->cpu != 0))
			continue;
		cpu_core_config *c = s_cpus[w->cpu].cfg;
		if (!c || !c->write)
			continue;
		if (c->open)
			c->open(s_cpus[w->cpu].n);
		for (int b = 0; b < w->bytes; b++)
			c->write(w->addr + b, (UINT8)(w->value >> (8 * b)));
		if (c->close)
			c->close();
	}
}

// At each frame end (main thread). Does nothing if no cheat is on (no hash impact)
extern "C" void rei_cheats_apply(void)
{
	for (int i = 0; i < s_ncheats; i++) {
		cheat *c = &s_cheats[i];
		if (c->sel <= 0 || c->sel >= c->nopts || !c->opt[c->sel].count)
			continue;
		if (c->type == 1 && c->applied)
			continue;
		apply_opt(&c->opt[c->sel]);
		c->applied = 1;
	}
}

extern "C" int rei_core_cheat_count(void) { return s_ncheats; }
extern "C" const char *rei_core_cheat_name(int i) { return (i >= 0 && i < s_ncheats) ? s_cheats[i].name : ""; }
extern "C" int rei_core_cheat_options(int i) { return (i >= 0 && i < s_ncheats) ? s_cheats[i].nopts : 0; }
extern "C" const char *rei_core_cheat_option_name(int i, int o)
{
	return (i >= 0 && i < s_ncheats && o >= 0 && o < s_cheats[i].nopts) ? s_cheats[i].opt[o].name : "";
}
extern "C" int rei_core_cheat_get(int i) { return (i >= 0 && i < s_ncheats) ? s_cheats[i].sel : 0; }
extern "C" void rei_core_cheat_set(int i, int o)
{
	if (i < 0 || i >= s_ncheats || o < 0 || o >= s_cheats[i].nopts)
		return;
	s_cheats[i].sel = o;
	s_cheats[i].applied = 0;
}
