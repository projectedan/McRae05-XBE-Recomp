#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define QEMU_FALLTHROUGH __attribute__((fallthrough))
#ifdef _WIN32
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
