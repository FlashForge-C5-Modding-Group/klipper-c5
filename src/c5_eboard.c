// Creator 5 eBoard detector, calibration, and vendor commands
//
// Copyright (C) 2026
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdint.h> // uint32_t
#include "board/irq.h" // irq_save
#include "board/misc.h" // timer_read_time
#include "c5_eboard.h" // c5_eboard_capture
#include "command.h" // DECL_COMMAND
#include "compiler.h" // barrier
#include "sched.h" // DECL_TASK

#define C5_RING_SIZE 11
#define C5_CALIBRATION_US 500
#define C5_STABILITY_CLOCK_DIVISOR 144000
#define C5_STABILITY_TIME_MS 2000
#define C5_LARGE_DELTA 150
#define C5_PA_SAMPLE_COUNT 2000

struct c5_detector {
    struct timer calibration_timer;
    uint32_t ring[C5_RING_SIZE];
    uint32_t current, baseline, reference, accumulator;
    uint32_t stable_start_ms, mad, filtered, signed_delta;
    uint16_t moderate_count, large_count;
    int16_t effective_threshold;
    uint8_t ring_count, ring_index, assert_count, clear_count;
    uint8_t eddy_state, immediate, capture_pending;
    uint8_t ready, normal_mode, calibration_active;
};

struct c5_pa_state {
    uint32_t action, verdict;
    volatile uint8_t pending;
    volatile uint16_t index;
    volatile int32_t samples[C5_PA_SAMPLE_COUNT];
};

static struct c5_detector eboard = {
    .effective_threshold = -20,
    .eddy_state = 1,
};
static struct c5_pa_state pa;
static struct task_wake classifier_wake;

static uint32_t
c5_abs_s32_wrapped(uint32_t value)
{
    return value & 0x80000000 ? 0u - value : value;
}

static uint16_t
c5_threshold_magnitude(int16_t threshold)
{
    uint16_t bits = threshold;
    return threshold < 0 ? (uint16_t)(0u - bits) : bits;
}

static void
c5_full_reset(void)
{
    uint32_t current = eboard.current;
    eboard.baseline = current;
    eboard.reference = current;
    eboard.accumulator = current << 2;
    eboard.ring_count = 0;
    eboard.ring_index = 0;
    eboard.moderate_count = 0;
    eboard.large_count = 0;
    eboard.stable_start_ms = 0;
    eboard.filtered = 0;
    eboard.assert_count = 0;
    eboard.clear_count = 0;
    eboard.immediate = 0;
    eboard.signed_delta = 0;
    eboard.eddy_state = 1;
    eboard.calibration_active = 1;
    eboard.ready = 1;
}

void
c5_eboard_capture(uint16_t interval)
{
    irqstatus_t flag = irq_save();
    eboard.current = interval;
    eboard.capture_pending = 1;
    sched_wake_task(&classifier_wake);
    irq_restore(flag);
}

uint8_t
c5_eboard_eddy_state(void)
{
    return eboard.eddy_state;
}

void
c5_eboard_arm(void)
{
    irqstatus_t flag = irq_save();
    eboard.eddy_state = 1;
    eboard.filtered = 0;
    eboard.assert_count = 0;
    eboard.clear_count = 0;
    eboard.immediate = 0;
    eboard.signed_delta = 0;
    irq_restore(flag);
}

static uint32_t
c5_select_calibration(uint32_t sample)
{
    if (!eboard.normal_mode) {
        eboard.moderate_count = 0;
        eboard.large_count = 0;
        return sample;
    }

    uint16_t difference = c5_abs_s32_wrapped(sample - eboard.reference);
    uint16_t limit = c5_threshold_magnitude(eboard.effective_threshold);
    if (difference <= limit && difference <= C5_LARGE_DELTA) {
        eboard.moderate_count = 0;
        eboard.large_count = 0;
        return sample;
    }
    if (difference <= C5_LARGE_DELTA) {
        eboard.moderate_count++;
        eboard.large_count = 0;
        if (eboard.moderate_count == 200) {
            eboard.moderate_count = 0;
            eboard.baseline = sample;
            eboard.reference = sample;
            return sample;
        }
        return eboard.baseline;
    }

    eboard.large_count++;
    if (eboard.large_count == 15) {
        eboard.immediate = 1;
        eboard.large_count = 0;
    }
    return sample;
}

static void
c5_sort(uint32_t *values)
{
    for (uint_fast8_t i = 1; i < C5_RING_SIZE; i++) {
        uint32_t value = values[i];
        uint_fast8_t position = i;
        while (position && values[position - 1] > value) {
            values[position] = values[position - 1];
            position--;
        }
        values[position] = value;
    }
}

static void
c5_compute_statistics(uint32_t *median, uint32_t *mad)
{
    uint32_t sorted[C5_RING_SIZE];
    for (uint_fast8_t i = 0; i < C5_RING_SIZE; i++)
        sorted[i] = eboard.ring[i];
    c5_sort(sorted);
    *median = sorted[C5_RING_SIZE / 2];
    for (uint_fast8_t i = 0; i < C5_RING_SIZE; i++)
        sorted[i] = eboard.ring[i] >= *median
            ? eboard.ring[i] - *median : *median - eboard.ring[i];
    c5_sort(sorted);
    *mad = sorted[C5_RING_SIZE / 2];
}

static uint32_t
c5_integer_sqrt(uint32_t mean)
{
    uint32_t x = mean;
    if (!mean)
        return 0;
    uint32_t y = (mean + 1u) >> 1;
    while (x > y) {
        x = y;
        y = ((x ? mean / x : 0u) + x) >> 1;
    }
    return x;
}

static uint32_t
c5_compute_rms(uint32_t median)
{
    uint64_t sum = 0;
    for (uint_fast8_t i = 0; i < C5_RING_SIZE; i++) {
        uint32_t difference = eboard.ring[i] - median;
        uint32_t square = difference * difference;
        sum += (int64_t)(int32_t)square;
    }
    return c5_integer_sqrt((uint32_t)(sum / C5_RING_SIZE));
}

static int16_t
c5_derive_threshold(uint32_t rms, uint32_t mad)
{
    uint32_t mad_noise = mad + (mad >> 1);
    uint32_t noise = rms > mad_noise ? rms : mad_noise;
    if (!noise)
        noise = 1;
    int16_t threshold = (int16_t)(uint16_t)(0u - 3u * noise);
    if (threshold < -60)
        return -60;
    if (threshold > -20)
        return -20;
    return threshold;
}

static void
c5_finish_calibration(uint32_t median, uint32_t mad)
{
    uint32_t rms = c5_compute_rms(median);
    int16_t threshold = c5_derive_threshold(rms, mad);
    uint32_t limit = mad ? 3u * mad : 2u;
    for (uint_fast8_t i = 0; i < eboard.ring_count; i++) {
        uint32_t difference = eboard.ring[i] >= median
            ? eboard.ring[i] - median : median - eboard.ring[i];
        if (difference > limit)
            eboard.ring[i] = median;
    }

    eboard.baseline = eboard.accumulator >> 2;
    eboard.reference = eboard.baseline;
    eboard.effective_threshold = threshold;
    eboard.calibration_active = 0;
    eboard.ring_count = 0;
    eboard.ring_index = 0;
    eboard.stable_start_ms = 0;
    eboard.moderate_count = 0;
    eboard.large_count = 0;
    eboard.ready = 1;
}

static void
c5_process_calibration(void)
{
    irqstatus_t flag = irq_save();
    uint8_t pending = eboard.capture_pending;
    uint32_t sample = eboard.current;
    if (pending)
        eboard.capture_pending = 0;
    irq_restore(flag);

    if (pending) {
        uint32_t selected = c5_select_calibration(sample);
        eboard.ring[eboard.ring_index++] = selected;
        if (eboard.ring_index == C5_RING_SIZE)
            eboard.ring_index = 0;
        if (eboard.ring_count < C5_RING_SIZE)
            eboard.ring_count++;

        uint32_t now_ms = timer_read_time() / C5_STABILITY_CLOCK_DIVISOR;
        if (!eboard.calibration_active) {
            if (eboard.ring_count == C5_RING_SIZE) {
                uint32_t median, mad;
                c5_compute_statistics(&median, &mad);
                eboard.accumulator = median * 4u;
                eboard.calibration_active = 1;
                eboard.stable_start_ms = now_ms;
            }
        } else {
            eboard.accumulator = selected + ((3u * eboard.accumulator) >> 2);
            uint32_t median;
            c5_compute_statistics(&median, &eboard.mad);
            if (eboard.mad > 2) {
                eboard.stable_start_ms = 0;
            } else {
                if (!eboard.stable_start_ms)
                    eboard.stable_start_ms = now_ms;
                if (now_ms - eboard.stable_start_ms >= C5_STABILITY_TIME_MS)
                    c5_finish_calibration(median, eboard.mad);
            }
        }
    }

    if (eboard.ready && !eboard.normal_mode) {
        eboard.normal_mode = 1;
        eboard.reference = eboard.baseline;
    }
}

static uint_fast8_t
c5_calibration_event(struct timer *timer)
{
    c5_process_calibration();
    timer->waketime += timer_from_us(C5_CALIBRATION_US);
    return SF_RESCHEDULE;
}

static void
c5_classify_sample(void)
{
    irqstatus_t flag = irq_save();
    uint32_t current = eboard.current;
    uint8_t immediate = eboard.immediate;
    uint8_t raw = eboard.eddy_state;
    eboard.immediate = 0;
    if ((uint32_t)(current - 1u) > 0xfffeu) {
        irq_restore(flag);
        return;
    }
    if (immediate) {
        eboard.eddy_state = 0;
        eboard.assert_count = 0;
        eboard.clear_count = 0;
        irq_restore(flag);
        return;
    }

    uint32_t delta = current - eboard.baseline;
    eboard.signed_delta = delta;
    uint32_t error = (delta << 4) - eboard.filtered;
    eboard.filtered += (uint32_t)((int32_t)error >> 2);
    int32_t metric = (int32_t)eboard.filtered >> 4;
    int32_t threshold = eboard.effective_threshold;
    if (metric <= threshold) {
        eboard.assert_count = 0;
        if (++eboard.clear_count >= 4) {
            eboard.clear_count = 4;
            eboard.eddy_state = 0;
        }
    } else if (metric >= (int16_t)(eboard.effective_threshold + 2)) {
        eboard.clear_count = 0;
        if (++eboard.assert_count >= 3) {
            eboard.assert_count = 3;
            eboard.eddy_state = 1;
        }
    } else if (raw) {
        eboard.clear_count = 0;
    } else {
        eboard.assert_count = 0;
    }
    irq_restore(flag);
}

void
c5_eboard_pa_receive(volatile uint8_t *frame, uint16_t remaining)
{
    static const uint8_t prefix[9] = { 0x05, 0x00, 0x41, 0xcf, 0x05,
                                       0xff, 0x41, 0x00, 0x00 };
    if (remaining == 15 || !pa.pending)
        return;
    uint_fast8_t i;
    for (i = 0; i < sizeof(prefix); i++)
        if (frame[i] != prefix[i])
            break;
    if (i == sizeof(prefix)) {
        uint16_t index = pa.index;
        pa.samples[index] = ((uint16_t)frame[9] << 8) | frame[10];
        index++;
        pa.index = index > 1989 ? 1990 : index;
    }
    for (i = 0; i < 15; i++)
        frame[i] = 0;
}

void
c5_eboard_task(void)
{
    if (pa.pending == 1 && (uint8_t)pa.action != 11) {
        if (c5_eboard_pa_matches(pa.samples, C5_PA_SAMPLE_COUNT))
            pa.verdict = 9;
        pa.pending = 0;
    }
    if (sched_check_wake(&classifier_wake))
        c5_classify_sample();
}
DECL_TASK(c5_eboard_task);

void
c5_eboard_init(void)
{
    eboard.calibration_timer.func = c5_calibration_event;
    eboard.calibration_timer.waketime = (timer_read_time()
                                         + timer_from_us(C5_CALIBRATION_US));
    sched_add_timer(&eboard.calibration_timer);
}

void
command_get_mcu_version(uint32_t *args)
{
    (void)args;
    sendf("mcu_version year=%u date=%u version=%u", 2026u, 919u, 2u);
}
DECL_COMMAND(command_get_mcu_version, "get_mcu_version");

void
command_set_trigger_threshold(uint32_t *args)
{
    int32_t threshold = (int32_t)args[0];
    eboard.effective_threshold = (int16_t)(uint16_t)threshold;
    sendf("trigger_threshold threshold=%i", threshold);
}
DECL_COMMAND(command_set_trigger_threshold,
             "set_trigger_threshold threshold=%i");

void
command_get_basic_param(uint32_t *args)
{
    (void)args;
    irqstatus_t flag = irq_save();
    uint32_t old_baseline = eboard.baseline;
    uint32_t current = eboard.current;
    uint32_t reserve = c5_abs_s32_wrapped(current - old_baseline);
    c5_full_reset();
    irq_restore(flag);
    sendf("param_value value=%u reserve=%u", old_baseline, reserve);
}
DECL_COMMAND(command_get_basic_param, "get_basic_param num=%u");

void
command_remove_peel(uint32_t *args)
{
    (void)args;
    irqstatus_t flag = irq_save();
    int32_t value = (int32_t)eboard.signed_delta;
    irq_restore(flag);
    sendf("peel_data value=%i", value);
}
DECL_COMMAND(command_remove_peel, "remove_peel action=%u");

void
command_pa_action(uint32_t *args)
{
    uint32_t action = args[0];
    pa.action = action;
    if (action == 11) {
        pa.pending = 1;
        pa.verdict = 0;
        pa.index = 0;
        for (uint_fast16_t i = 0; i < sizeof(pa.samples); i++)
            ((volatile uint8_t *)pa.samples)[i] = 0;
        barrier();
        c5_eboard_set_pa_mode(1);
    } else {
        c5_eboard_set_pa_mode(0);
        if (pa.pending)
            sched_wake_tasks();
    }
}
DECL_COMMAND(command_pa_action, "pa_action action=%u pc=%u");

void
command_get_emcu_pa_value(uint32_t *args)
{
    (void)args;
    sendf("pa_value value=%u", pa.verdict);
}
DECL_COMMAND(command_get_emcu_pa_value, "get_emcu_pa_value");

DECL_CONSTANT("ADC_MAX", 4095);
