/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_CALCULATOR_H
#define WIIDESK_CALCULATOR_H
#include <stddef.h>
/* Bounded expression evaluator, radians; no scripting or external commands. */
int calculator_eval(const char *, double *, char *, size_t);
#endif
