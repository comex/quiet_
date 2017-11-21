// simple sscanf
#pragma once
#include "types.h"
__attribute__((format(scanf, 2, 3))) int
ssscanf(const char *s, const char *format, ...);

// extras:
void s_hex_encode(char *out, const char *in, size_t in_len);
bool s_parse_hex(const char *in, void *out, size_t out_len);
