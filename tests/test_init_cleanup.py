#!/usr/bin/env python3
"""Fault-inject the real init/exit control flow using a host resource model.

No kernel or MMIO access. This checks resource lifetime, not kernel concurrency
or hardware behavior. Pass --source to check an older driver for regression.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

STUBS = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#define __init
#define __exit
#define GFP_KERNEL 0
#define HZ 100
#define TGPIO_MIN_MMIO_SIZE 0x38
#define TGPIO_WATCHDOG_PERIOD_S 10
#define CSID_X86_ART 1
#define TGPIO_REQUIRED 1
#define TGPIO_CLOCK_REALTIME 0
#define TGPIO_CLOCK_PHC 1
#define TGPIO_CLOCK_ART 2
#define pr_err(...) ((void)0)
#define pr_info(...) ((void)0)
#define seqlock_init(x) ((void)(x))
#define INIT_DELAYED_WORK(w, fn) ((w)->initialized = true)
struct delayed_work { bool initialized, pending; };
struct tgpio_device {
    int phc_seqlock;
    struct delayed_work poll_work, art_refine_work, watchdog_work;
    void *ptp_clock;
    bool mapped, pps, output;
};
struct tgpio_clock_mode_result { int status, mode; };
struct tgpio_timestamp_mode_result { int status, mode; };
struct tgpio_output_polarity_result { int status, polarity; };
static struct tgpio_device *tgpio;
static int fail_at, selected_mode, checks;
static int clock_mode, timestamp_mode, output_polarity;
static unsigned long mmio_size = TGPIO_MIN_MMIO_SIZE, art_frequency;
static const char *clock_mode_param, *timestamp_mode_param, *output_polarity_param;
static bool registered, observable;
static void require(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "mode=%d failure=%d: %s\n", selected_mode, fail_at, what);
        exit(1);
    }
    checks++;
}
static void *kzalloc(size_t size, int flags) {
    (void)flags;
    return fail_at == 1 ? NULL : calloc(1, size);
}
static void schedule_delayed_work(struct delayed_work *w, int delay) {
    (void)delay;
    require(w->initialized, "scheduled uninitialized work");
    w->pending = true;
}
static void cancel_delayed_work_sync(struct delayed_work *w) {
    require(w->initialized, "cancelled uninitialized work");
    w->pending = false;
}
static void kfree(struct tgpio_device *d) {
    require(!d->watchdog_work.pending, "freed memory with watchdog still pending");
    require(!d->art_refine_work.pending, "freed memory with refinement still pending");
    require(!d->poll_work.pending && !d->output, "freed active I/O");
    require(!d->mapped && !d->pps && !registered && !observable,
            "freed memory with published resources");
    free(d);
}
static struct tgpio_clock_mode_result tgpio_parse_clock_mode(const char *s) {
    (void)s;
    return (struct tgpio_clock_mode_result){fail_at == 2 ? -EINVAL : 0, selected_mode};
}
static struct tgpio_timestamp_mode_result tgpio_parse_timestamp_mode(const char *s) {
    (void)s;
    return (struct tgpio_timestamp_mode_result){fail_at == 3 ? -EINVAL : 0, 0};
}
static struct tgpio_output_polarity_result tgpio_parse_output_polarity(const char *s) {
    (void)s;
    return (struct tgpio_output_polarity_result){fail_at == 4 ? -EINVAL : 0, 0};
}
static int tgpio_configure_mmio_blocks(struct tgpio_device *d) {
    d->mapped = true; /* also model partial mapping failure */
    return fail_at == 5 ? -EBUSY : 0;
}
static void tgpio_pps_register(struct tgpio_device *d) { d->pps = true; }
static void tgpio_pps_unregister(struct tgpio_device *d) {
    require(!d->poll_work.pending, "PPS released while capture work is active");
    d->pps = false;
}
static int tgpio_validate_persistent_operations(struct tgpio_device *d) {
    (void)d; return fail_at == 6 ? -EINVAL : 0;
}
static void tgpio_resolve_do_adjtimex(void) {}
static bool tgpio_clock_uses_model(void) { return clock_mode != TGPIO_CLOCK_REALTIME; }
static bool timekeeping_clocksource_has_base(int id) { (void)id; return fail_at != 7; }
static int tgpio_art_init_clock(struct tgpio_device *d) {
    if (fail_at == 8) return -ENODEV;
    schedule_delayed_work(&d->art_refine_work, 10 * HZ);
    return 0;
}
static int tgpio_phc_init_clock(struct tgpio_device *d) {
    (void)d; return fail_at == 8 ? -ENODEV : 0;
}
static void tgpio_probe_art_parameters_for_status(void) {}
static int tgpio_needs_art_frequency(struct tgpio_device *d) { (void)d; return 1; }
static int tgpio_needs_art_base_clock(struct tgpio_device *d) { (void)d; return 1; }
static int tgpio_resolve_art_frequency(void) { return fail_at == 8 ? -ENODEV : 0; }
static int tgpio_register_ptp_clock(struct tgpio_device *d) {
    if (fail_at == 9) return -ENODEV;
    d->ptp_clock = d; registered = true; return 0;
}
static void ptp_clock_unregister(void *p) { (void)p; registered = false; }
static void tgpio_observability_init(struct tgpio_device *d) { (void)d; observable = true; }
static void tgpio_observability_exit(void) { observable = false; }
static int tgpio_start_persistent_operations(struct tgpio_device *d) {
    d->output = true;
    schedule_delayed_work(&d->poll_work, 1);
    return fail_at == 10 ? -ENODEV : 0;
}
static void tgpio_disable_mmio_blocks(struct tgpio_device *d) {
    require(!registered, "PTP callbacks can still restart I/O");
    require(!d->art_refine_work.pending && !d->watchdog_work.pending,
            "background work still accesses I/O during teardown");
    cancel_delayed_work_sync(&d->poll_work);
    d->output = false;
}
static void tgpio_unmap_mmio_blocks(struct tgpio_device *d) {
    require(!d->output && !d->poll_work.pending, "unmapped active I/O");
    d->mapped = false;
}
'''
MAIN = r'''
int main(void) {
    int scenarios = 0;
    for (selected_mode = 0; selected_mode < 3; selected_mode++) {
        for (int scenario = 1; scenario <= 11; scenario++) {
            fail_at = scenario % 11;
            if (selected_mode == TGPIO_CLOCK_REALTIME && fail_at == 7) continue;
            int ret = tgpio_input_init();
            if (fail_at) require(ret < 0, "injected failure unexpectedly succeeded");
            else {
                require(ret == 0 && tgpio, "normal init failed");
                require(tgpio->watchdog_work.pending, "watchdog not started on success");
                require(tgpio->art_refine_work.pending ==
                        (selected_mode != TGPIO_CLOCK_PHC), "wrong refinement lifetime");
                tgpio_input_exit();
            }
            require(!tgpio && !registered && !observable, "cleanup incomplete");
            scenarios++;
        }
    }
    printf("PASS: %d fault/success scenarios, %d resource assertions\n", scenarios, checks);
    return 0;
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'src/tgpio-ptp-input.c')
    args = parser.parse_args()
    text = args.source.read_text()
    functions = text[text.index('static int __init tgpio_input_init(void)'):text.index('module_init(tgpio_input_init);')]
    with tempfile.TemporaryDirectory(prefix='tgpio-init-test-') as tmp:
        source = Path(tmp) / 'test.c'
        binary = Path(tmp) / 'test'
        source.write_text(STUBS + functions + MAIN)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
