#ifndef PICO_AUDIO_STANDALONE_TOOLCHAIN_COMPAT_H
#define PICO_AUDIO_STANDALONE_TOOLCHAIN_COMPAT_H

#include <sys/cdefs.h>

#ifndef __printflike
#define __printflike(format_index, first_argument)
#endif

int audio_noop_printf(const char *format, ...);

#endif
