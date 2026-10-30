/* ReiREM PSP: sound side on the Media Engine (src/core/shim/rei_me.h, psp/docs/DECISIONS.md PSP-ADR-003).
 * Inserted into pspdev's linkfile.prx before .data. Collects every variable the sound side writes into one
 * 64-byte aligned block (.rei_me), and the main<->sound shared variables (only accessed uncached) into
 * another (.rei_shared), so that no cache line is written by both CPUs. Input sections are claimed here
 * before the main script's .data/.bss rules see them. */
SECTIONS
{
  .rei_me ALIGN(64) :
  {
    __rei_me_start = .;
    *(.rei_me_data)
    *librei_core.a:v25.cpp.obj(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON)
    *librei_core.a:ym2151.c.obj(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON)
    *librei_core.a:burn_ym2151.cpp.obj(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON)
    *librei_core.a:iremga20.cpp.obj(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON)
    *librei_core.a:timer.cpp.obj(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON)
    *librei_core.a:burn_sound.cpp.obj(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON)
    *librei_core.a:burn_sound_c.cpp.obj(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON)
    *(.rei_me_local)
    . = ALIGN(64);
    __rei_me_end = .;
  }
  .rei_shared ALIGN(64) :
  {
    __rei_shared_start = .;
    *(.rei_me_shared)
    . = ALIGN(64);
    __rei_shared_end = .;
  }
  /* globals only the renderer writes while drawing is deferred (REI_RDR, src/core/shim/rei_defer.h): other boards
   * draw on the ME, so these must not share a cache line with anything the main CPU writes */
  .rei_rdr ALIGN(64) :
  {
    __rei_rdr_start = .;
    *(.rei_rdr_data)
    /* the Media Engine's YM2151 copy (burn_ym2151.cpp REI_YM_ME): only the ME writes it while it runs */
    *librei_core.a:ym2151_main.c.obj(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON)
    . = ALIGN(64);
    __rei_rdr_end = .;
  }
}
INSERT BEFORE .data;

/* Sound side code at a fixed place in front of .text: the Media Engine runs it from its own small caches,
 * so its speed depends on where the hot functions land. Collected here, it does not move when unrelated
 * code changes (PSP perf PP10). */
SECTIONS
{
  .rei_me_text ALIGN(64) :
  {
    /* (experiment: shift the whole module in cache position - CMake REI_PSP_ME_PAD, default 0) */
    . += DEFINED(REI_ME_PAD) ? REI_ME_PAD : 0;
    *librei_core.a:v25.cpp.obj(.text .text.*)
    *librei_core.a:ym2151.c.obj(.text .text.*)
    *librei_core.a:burn_ym2151.cpp.obj(.text .text.*)
    *librei_core.a:iremga20.cpp.obj(.text .text.*)
    *librei_core.a:timer.cpp.obj(.text .text.*)
    *librei_core.a:burn_sound.cpp.obj(.text .text.*)
    *librei_core.a:burn_sound_c.cpp.obj(.text .text.*)
    . = ALIGN(64);
  }
  /* then the rest of the core (d_m92.cpp holds sound side code the ME runs), the V33 interpreter and its
   * fast paths last (nec.cpp REI_FAST_FN): neither frontend nor interpreter changes move the code the ME runs,
   * and fast path changes do not move the interpreter (a changed layout alone moved play frames by up to
   * 1ms, PSP perf PP10) */
  .rei_core_text ALIGN(64) :
  {
    *librei_core.a:d_m92.cpp.obj(.text .text.*)
    *librei_core.a:burn_shim.cpp.obj(.text .text.*)
    *librei_core.a:nec_intf.cpp.obj(.text .text.*)
    *librei_core.a:pic8259.cpp.obj(.text .text.*)
    *librei_core.a:irem_cpu.cpp.obj(.text .text.*)
    *librei_core.a:burn_memory.cpp.obj(.text .text.*)
    *librei_core.a:msm6295.cpp.obj(.text .text.*)
    *librei_core.a:ym2151_main.c.obj(.text .text.*)
    *librei_core.a:zipfile.c.obj(.text .text.*)
    . = ALIGN(64);
    INCLUDE nec_hot.ld
    *(SORT_BY_NAME(.nbbline_text.*))
    *librei_core.a:nec.cpp.obj(.text .text.*)
    . = ALIGN(64);
    *(.reifast_text)
    . = ALIGN(64);
    /* Other boards: sound Z80 interpreter and hot drivers also get fixed slots - placed last so the M92 layout above stays. Each layout change
     * shifted I-cache conflicts, ±2ms jitter (PSP PP28) */
    *librei_core.a:z80.cpp.obj(.text .text.*)
    *librei_core.a:z80_intf.cpp.obj(.text .text.*)
    *librei_core.a:d_m72.cpp.obj(.text .text.*)
    *librei_core.a:d_m90.cpp.obj(.text .text.*)
    *librei_core.a:d_vigilant.cpp.obj(.text .text.*)
    *librei_core.a:dac.cpp.obj(.text .text.*)
  }
}
INSERT BEFORE .text;

/* the V33 fast paths' tables (nec.cpp REI_FAST_DATA) in a block of fixed size in front of .sbss/.bss, so growing
 * them moves no other data (the block is in the file: data sections after .bss would pull the whole .bss in) */
SECTIONS
{
  .reifast_data ALIGN(64) :
  {
    __reifast_start = .;
    *(.reifast_data)
    ASSERT(. <= __reifast_start + 0x90000, "reifast_data over 576KB");
    . = __reifast_start + 0x90000;
  }
}
INSERT BEFORE .sbss;
