/**
  ******************************************************************************
  * @file    syscalls.c
  * @brief   Minimal system-call stubs for newlib/libc.
  *
  * W tym projekcie nie wysylamy tekstu bezposrednio z _write() przez UART.
  * Gdy jakis kod przypadkowo uzyje printf(), tekst trafia do centralnego
  * loggera FreeRTOS. Logger wrzuca komunikat do kolejki i dopiero DebugTask
  * obsluguje fizyczna transmisje UART/RS.
  ******************************************************************************
  */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>

#include "debug_logger.h"
#include "stm32h7xx_hal.h"

#define SYSCALLS_TEST_UNIX_TIME_UTC 1783263600L

char *__env[1] = { 0 };
char **environ = __env;

void initialise_monitor_handles(void)
{
}

int _getpid(void)
{
    return 1;
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

void _exit(int status)
{
    (void)status;
    while (1)
    {
    }
}

int _read(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;

    errno = EIO;
    return -1;
}

int _write(int file, char *ptr, int len)
{
    char text[128];
    int copy_len = len;

    (void)file;

    if ((ptr == NULL) || (len <= 0))
    {
        return 0;
    }

    if (copy_len >= (int)sizeof(text))
    {
        copy_len = (int)sizeof(text) - 1;
    }

    memcpy(text, ptr, (size_t)copy_len);
    text[copy_len] = '\0';

    Debug_LogLine(DEBUG_SRC_SYSTEM, text);

    /* Zwracamy oryginalny len, zeby printf nie probowal ponawiac zapisu. */
    return len;
}

int _close(int file)
{
    (void)file;
    return -1;
}

int _fstat(int file, struct stat *st)
{
    (void)file;

    if (st != NULL)
    {
        st->st_mode = S_IFCHR;
    }

    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

int _lseek(int file, int ptr, int dir)
{
    (void)file;
    (void)ptr;
    (void)dir;
    return 0;
}

int _open(char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    errno = ENOENT;
    return -1;
}

int _wait(int *status)
{
    (void)status;
    errno = ECHILD;
    return -1;
}

int _unlink(char *name)
{
    (void)name;
    errno = ENOENT;
    return -1;
}

int _times(struct tms *buf)
{
    (void)buf;
    return -1;
}

int _stat(char *file, struct stat *st)
{
    (void)file;

    if (st != NULL)
    {
        st->st_mode = S_IFCHR;
    }

    return 0;
}

int _link(char *old, char *new)
{
    (void)old;
    (void)new;
    errno = EMLINK;
    return -1;
}

int _fork(void)
{
    errno = EAGAIN;
    return -1;
}

int _execve(char *name, char **argv, char **env)
{
    (void)name;
    (void)argv;
    (void)env;
    errno = ENOMEM;
    return -1;
}

int _gettimeofday(struct timeval *tv, void *tzvp)
{
    (void)tzvp;

    if (tv != NULL)
    {
        uint32_t tick_ms = HAL_GetTick();

        /*
         * Testowy czas bazowy: 2026-07-05 15:00:00 UTC.
         * Docelowo powinno to zostac zastapione RTC albo SNTP.
         */
        tv->tv_sec = SYSCALLS_TEST_UNIX_TIME_UTC + (time_t)(tick_ms / 1000U);
        tv->tv_usec = (suseconds_t)((tick_ms % 1000U) * 1000U);
    }

    return 0;
}
