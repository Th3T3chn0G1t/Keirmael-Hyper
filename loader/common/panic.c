#include <stdarg.h>
#include "panic.h"
#include "log.h"

void panic(const char *reason, ...)
{
    va_list vlist;
    va_start(reason, vlist);
    vprintlvl(LOG_LEVEL_ERR, reason, vlist);
    va_end(vlist);

    for (;;);
}

void oops(const char *reason, ...)
{
    print_err("Oops!\n");

    va_list vlist;
    va_start(reason, vlist);
    vprintlvl(LOG_LEVEL_ERR, reason, vlist);
    va_end(vlist);

    for (;;);
}