// Built-in UDP logger. No dependency on plugins (PrincessLog etc.).
#ifdef REI_ENABLE_LOG

#include "platform/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include "platform/timer.h"

#ifndef REI_LOG_HOST
#define REI_LOG_HOST "255.255.255.255"
#endif

#define NET_MEM_SIZE (128 * 1024)
#define LINE_MAX_LEN 512

static char s_net_mem[NET_MEM_SIZE] __attribute__((aligned(64)));
static int s_sock = -1;
static int s_net_inited;
static SceNetSockaddrIn s_addr;

void rei_log_init(void)
{
	if (s_sock >= 0)
		return;

	if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED)
		sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

	SceNetInitParam param;
	param.memory = s_net_mem;
	param.size = sizeof(s_net_mem);
	param.flags = 0;
	int r = sceNetInit(&param);
	if (r < 0 && r != (int)SCE_NET_ERROR_EBUSY)
		return; // silently disable logging if the network is unavailable
	s_net_inited = (r >= 0);
	sceNetCtlInit();

	s_sock = sceNetSocket("rei_log", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
	if (s_sock < 0)
		return;

	int on = 1;
	sceNetSetsockopt(s_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &on, sizeof(on));

	memset(&s_addr, 0, sizeof(s_addr));
	s_addr.sin_len = sizeof(s_addr);
	s_addr.sin_family = SCE_NET_AF_INET;
	s_addr.sin_port = sceNetHtons(REI_LOG_PORT);
	sceNetInetPton(SCE_NET_AF_INET, REI_LOG_HOST, &s_addr.sin_addr);

	rei_log_printf("log: start -> %s:%d", REI_LOG_HOST, REI_LOG_PORT);
}

void rei_log_shutdown(void)
{
	if (s_sock >= 0) {
		sceNetSocketClose(s_sock);
		s_sock = -1;
	}
	sceNetCtlTerm();
	if (s_net_inited) {
		sceNetTerm();
		s_net_inited = 0;
	}
}

// safe to call from multiple threads (line buffer on the stack, one sendto per message)
void rei_log_printf(const char *fmt, ...)
{
	if (s_sock < 0)
		return;

	char line[LINE_MAX_LEN];
	uint64_t t = rei_time_us();
	int n = snprintf(line, sizeof(line), "[%7u.%03u] ",
		(unsigned)(t / 1000000), (unsigned)(t / 1000 % 1000));

	va_list ap;
	va_start(ap, fmt);
	int m = vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
	va_end(ap);
	if (m < 0)
		return;
	n += m;
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';

	sceNetSendto(s_sock, line, n, 0, (SceNetSockaddr *)&s_addr, sizeof(s_addr));
}

#endif // REI_ENABLE_LOG
