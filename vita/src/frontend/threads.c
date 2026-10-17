#include "frontend/threads.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "frontend/audio.h"
#include "rei_core.h"
#include "platform/log.h"
#include "platform/perf.h"

// Sound thread priority: just below the audio output thread (64). Higher than main (default 160)
#define SOUND_PRIORITY   80
#define SOUND_STACK_SIZE (256 * 1024)

static SceUID s_sound = -1;
static volatile int s_running;

void rei_threads_pin_main(void)
{
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
}

static int sound_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	const int alen = rei_core_audio_len();
	int16_t *buf = (int16_t *)calloc((size_t)alen * 2 + 16, sizeof(int16_t));

	uint64_t prev = rei_time_us();
	while (s_running) {
		memset(buf, 0, (size_t)alen * 2 * sizeof(int16_t));
		// run one frame's worth, following main's progress (includes time waiting for main)
		rei_core_sound_frame(buf);
		if (!s_running)
			break;
#ifdef REI_AUDIO_DUMP
		rei_audio_dump("ux0:data/reirem/dump_core.raw", 0, buf, alen);
#endif
		rei_audio_push(buf, alen);

		uint64_t now = rei_time_us();
		rei_perf_record(REI_PERF_SOUND, (uint32_t)(now - prev));
		prev = now;
	}
	free(buf);
	return sceKernelExitDeleteThread(0);
}

int rei_threads_start_sound(void)
{
	s_running = 1;
	s_sound = sceKernelCreateThread("rei_sound", sound_thread, SOUND_PRIORITY, SOUND_STACK_SIZE,
		0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (s_sound < 0) {
		REI_LOG("threads: sound thread create failed 0x%08x", s_sound);
		s_running = 0;
		return -1;
	}
	sceKernelStartThread(s_sound, 0, NULL);
	REI_LOG("threads: sound thread started on CPU1");
	return 0;
}

// Other boards: YM2151 synthesis worker thread (CPU1; core rei_core_ym_worker_run runs until rei_core_stop)
static SceUID s_ym = -1;
static int ym_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	rei_core_ym_worker_run();
	return sceKernelExitDeleteThread(0);
}

int rei_threads_start_ym(void)
{
	if (!rei_core_ym_worker())
		return -1;
	rei_core_ym_worker_enable(1);
	s_ym = sceKernelCreateThread("rei_ym", ym_thread, SOUND_PRIORITY, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (s_ym < 0) {
		REI_LOG("threads: ym thread create failed 0x%08x", s_ym);
		rei_core_ym_worker_enable(0);
		return -1;
	}
	sceKernelStartThread(s_ym, 0, NULL);
	REI_LOG("threads: YM2151 worker started on CPU1");
	return 0;
}

void rei_threads_stop(void)
{
	if (s_ym >= 0) {
		rei_core_ym_worker_enable(0); // (main takes over after synthesizing all pending writes)
		rei_core_stop();
		sceKernelWaitThreadEnd(s_ym, NULL, NULL);
		s_ym = -1;
	}
	if (s_sound < 0)
		return;
	s_running = 0;
	rei_core_stop();
	sceKernelWaitThreadEnd(s_sound, NULL, NULL);
	s_sound = -1;
}
