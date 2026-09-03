/*
 * main.c - bring-up and the main loop.
 *
 * Initialisation order is not arbitrary. The HAL comes up first because
 * everything else touches pins; DDM precedes CMIS because the memory map
 * publishes DDM's threshold defaults into page 02h at init; and the link
 * state machines come up last so that they see a settled world on their
 * first pass.
 */
#include "optilink/cli.h"
#include "optilink/cmis.h"
#include "optilink/ddm.h"
#include "optilink/eth.h"
#include "optilink/hal.h"
#include "optilink/i2c.h"
#include "optilink/link.h"
#include "optilink/optilink.h"
#include "optilink/sched.h"
#include "optilink/uart.h"

static void task_link(void *ctx)
{
    (void)ctx;
    link_service(hal_millis());
}

static void task_ddm(void *ctx)
{
    (void)ctx;
    ddm_sample();
}

static void task_cmis(void *ctx)
{
    (void)ctx;
    cmis_refresh();
}

int main(void)
{
    hal_init();
    uart_init(115200u);

    ddm_init();
    cmis_init();
    i2c_target_init(OPTILINK_I2C_ADDR);
    eth_reset_stats();
    link_init(hal_millis());
    cli_init();

    sched_init(hal_millis());
    /* Console first: a module that will not answer its debug port is a
     * module nobody can diagnose, so it outranks even the link. The link
     * machines run at 10 ms because that is what the 30 ms debounce budget
     * allows, and the two 100 ms jobs are just telemetry. */
    (void)sched_register("cli", cli_task, NULL, 1u, 0u);
    (void)sched_register("link", task_link, NULL, 10u, 1u);
    (void)sched_register("ddm", task_ddm, NULL, 100u, 2u);
    (void)sched_register("cmis", task_cmis, NULL, 100u, 3u);

    /* The banner is terminated the same way every command response is, so a
     * harness can use one read-until-sentinel routine for startup and for
     * everything after it. */
    uart_printf("fw=optilink version=%s ready\n", OPTILINK_VERSION_STR);
    uart_puts("ok\n");

    for (;;) {
        hal_wait_for_tick();
        sched_service(hal_millis());
    }
}
