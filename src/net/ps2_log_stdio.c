/**
 * Minimal rs2_log backend for standalone PS2 test/client ELFs.
 *
 * These targets do not link platform.c (which provides the full application
 * logger), so keep the same API and write to both EE stdout and a host file.
 */
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>

static FILE *log_file;
static int host_open_error;
static int relative_open_error;

/* ps2ip's bundled lwIP exposes a weak errno symbol that may be unaligned.
 * A strong EE-side definition keeps stdio error reporting aligned as well. */
int errno;

static void rs2_log_open_file(void)
{
    if (log_file)
        return;

    /* Early startup messages can arrive before SifLoadFileInit() has enabled
     * IOP file I/O. Retry on later messages instead of permanently giving up. */
    errno = 0;
    log_file = fopen("host:net_test.log", "w");
    if (!log_file) {
        host_open_error = errno;
        errno = 0;
        log_file = fopen("host:/net_test.log", "w");
        if (!log_file)
            host_open_error = errno;
    }
    if (!log_file) {
        errno = 0;
        log_file = fopen("net_test.log", "w");
        if (!log_file)
            relative_open_error = errno;
    }
}

int rs2_log_file_available(void)
{
    rs2_log_open_file();
    return log_file != NULL;
}

void rs2_log_file_open_errors(int *host_error, int *relative_error)
{
    rs2_log_open_file();
    if (host_error)
        *host_error = host_open_error;
    if (relative_error)
        *relative_error = relative_open_error;
}

void rs2_log(const char *format, ...)
{
    va_list args;

    va_start(args, format);

    va_list console_args;
    va_copy(console_args, args);
    vprintf(format, console_args);
    va_end(console_args);
    fflush(stdout);

    /* Address PCSX2 HostFS explicitly, then try the default device as a
     * fallback. Keep the file open and flush each message to survive hangs. */
    rs2_log_open_file();
    if (log_file) {
        va_list file_args;
        va_copy(file_args, args);
        vfprintf(log_file, format, file_args);
        va_end(file_args);
        fflush(log_file);
    }

    va_end(args);
}