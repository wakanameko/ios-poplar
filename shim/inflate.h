#pragma once
#include <stddef.h>
#include <stdint.h>

int inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);
