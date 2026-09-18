// SPDX-License-Identifier: GPL-2.0-only
#include "calculator.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct parser { const char *p; const char *error; unsigned depth; };
static void spaces(struct parser *p) { while (isspace((unsigned char)*p->p)) p->p++; }
static double expression(struct parser *);
static double unary(struct parser *);
static double atom(struct parser *p)
{
    spaces(p);
    if (++p->depth > 32) { p->error = "Expression nesting limit"; --p->depth; return 0; }
    double value = 0;
    if (*p->p == '(') {
        p->p++; value = expression(p); spaces(p);
        if (*p->p != ')') p->error = "Missing closing parenthesis"; else p->p++;
    } else if (isalpha((unsigned char)*p->p)) {
        char name[16]; size_t n = 0;
        while (isalpha((unsigned char)*p->p)) {
            if (n + 1 < sizeof(name)) name[n++] = *p->p;
            else p->error = "Unknown function";
            p->p++;
        }
        name[n] = 0; spaces(p);
        if (!strcmp(name, "pi")) value = acos(-1.0);
        else if (!strcmp(name, "e")) value = exp(1.0);
        else if (*p->p != '(') p->error = "Expected function(argument)";
        else {
            p->p++; double a = expression(p); spaces(p);
            if (*p->p != ')') p->error = "Missing closing parenthesis"; else p->p++;
            if (!strcmp(name, "sin")) value = sin(a);
            else if (!strcmp(name, "cos")) value = cos(a);
            else if (!strcmp(name, "tan")) value = tan(a);
            else if (!strcmp(name, "sqrt")) value = sqrt(a);
            else if (!strcmp(name, "ln")) value = log(a);
            else if (!strcmp(name, "log")) value = log10(a);
            else if (!strcmp(name, "exp")) value = exp(a);
            else if (!strcmp(name, "abs")) value = fabs(a);
            else p->error = "Unknown function";
        }
    } else {
        char *end; errno = 0; value = strtod(p->p, &end);
        if (end == p->p) p->error = "Expected a number";
        else if (errno == ERANGE) p->error = "Number out of range";
        p->p = end;
    }
    --p->depth; return value;
}
static double power(struct parser *p)
{
    double v = atom(p); spaces(p);
    if (!p->error && *p->p == '^') { p->p++; v = pow(v, unary(p)); }
    return v;
}
static double unary(struct parser *p)
{
    if (++p->depth > 32) { p->error = "Expression nesting limit"; --p->depth; return 0; }
    spaces(p); double v;
    if (*p->p == '+' || *p->p == '-') { char op = *p->p++; v = unary(p); if (op == '-') v = -v; }
    else v = power(p);
    --p->depth; return v;
}
static double product(struct parser *p)
{
    double v = unary(p); spaces(p);
    while (!p->error && (*p->p == '*' || *p->p == '/' || *p->p == '%')) {
        char op = *p->p++; double rhs = unary(p);
        if (op != '*' && rhs == 0) { p->error = "Division by zero"; return 0; }
        v = op == '*' ? v * rhs : op == '/' ? v / rhs : fmod(v, rhs); spaces(p);
    }
    return v;
}
static double expression(struct parser *p)
{
    double v = product(p); spaces(p);
    while (!p->error && (*p->p == '+' || *p->p == '-')) {
        char op = *p->p++; double rhs = product(p); v = op == '+' ? v + rhs : v - rhs; spaces(p);
    }
    return v;
}
int calculator_eval(const char *input, double *value, char *error, size_t size)
{
    struct parser p = {.p = input};
    if (strlen(input) > 256) p.error = "Expression exceeds 256 characters";
    double result = p.error ? 0 : expression(&p);
    if (!p.error && *p.p) p.error = "Unexpected character";
    if (!p.error && !isfinite(result)) p.error = "Result is outside the real finite range";
    if (p.error) { snprintf(error, size, "%s", p.error); return -1; }
    *value = result; if (size) error[0] = 0; return 0;
}
