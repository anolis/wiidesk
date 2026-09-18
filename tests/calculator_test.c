// SPDX-License-Identifier: GPL-2.0-only
#include "calculator.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void)
{
    const struct {const char *s; double v;} cases[] = {
        {"2+3*4",14}, {"(2+3)*4",20}, {"2^3^2",512}, {"-2^2",-4},
        {"2^-2",.25}, {"sqrt(81)+abs(-2)",11}, {"sin(pi/2)",1},
        {"log(100)+ln(e)",3}, {"5%2",1}, {"1e-3 + .5",.501}
    };
    char error[128]; double value;
    for (unsigned i=0; i<sizeof(cases)/sizeof(cases[0]); i++) {
        assert(!calculator_eval(cases[i].s,&value,error,sizeof(error)));
        assert(fabs(value-cases[i].v)<1e-10);
    }
    const char *bad[] = {"", "1/0", "sqrt(-1)", "2+", "(2", "foo(1)", "1;exit", "exp(9999)",
        "----------------------------------------1", "2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2^2"};
    for (unsigned i=0; i<sizeof(bad)/sizeof(bad[0]); i++) assert(calculator_eval(bad[i],&value,error,sizeof(error)));
    puts("PASS: calculator precedence, scientific functions, bounds and invalid input");
}
