#ifndef REI_PLATFORM_LOG_H
#define REI_PLATFORM_LOG_H

// Built-in UDP logger (docs/RULES.md 5-1)
// - Port 18194. Receive on PC with `nc -lu 18194` or `python tools/udp_log.py`.
// - Target address set at build time via REI_LOG_HOST. Broadcast if unset.
// - Without REI_ENABLE_LOG (release builds) all calls are compiled out.

#define REI_LOG_PORT 18194

#ifdef REI_ENABLE_LOG

void rei_log_init(void);
void rei_log_shutdown(void);
void rei_log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define REI_LOG(...) rei_log_printf(__VA_ARGS__)

#else

#define rei_log_init()     ((void)0)
#define rei_log_shutdown() ((void)0)
#define REI_LOG(...)       ((void)0)

#endif

#endif
