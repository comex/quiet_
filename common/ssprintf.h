#pragma once
#include "types.h"
#include "my_stdarg.h"

__attribute__((format(printf, 3, 0)))
int svsnprintf(char *buf, size_t len, const char *format, my_va_list *map);
__attribute__((format(printf, 3, 4)))
int ssnprintf(char *buf, size_t len, const char *format, ...);
