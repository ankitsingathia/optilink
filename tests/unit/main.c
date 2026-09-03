#include <stdio.h>

#include "suites.h"
#include "tinytest.h"

unsigned    tt_checks;
unsigned    tt_failures;
const char *tt_case = "?";

int main(void)
{
    suite_ringbuf();
    suite_sched();
    suite_ddm();
    suite_eth();
    suite_cmis();
    suite_link();

    printf("%u checks, %u failures\n", tt_checks, tt_failures);
    return (tt_failures == 0u) ? 0 : 1;
}
