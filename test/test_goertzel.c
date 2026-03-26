/**
 * Host-side unit test for the Goertzel algorithm.
 *
 * Build: gcc -o test_goertzel test_goertzel.c ../dsp/src/dsp_goertzel.c -lm
 * Run:   ./test_goertzel
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "dsp_goertzel.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 32000.0f
#define BLOCK_SIZE  32
#define TOLERANCE   0.01f

static int tests_passed = 0;
static int tests_failed = 0;

static void check(const char *name, float actual, float expected, float tol)
{
    float diff = fabsf(actual - expected);
    if (diff <= tol) {
        tests_passed++;
        printf("  PASS: %s = %.4f (expected %.4f)\n", name, actual, expected);
    } else {
        tests_failed++;
        printf("  FAIL: %s = %.4f (expected %.4f, diff %.4f)\n",
               name, actual, expected, diff);
    }
}

/**
 * Test 1: Pure sine at target frequency.
 * Goertzel should report the correct amplitude.
 */
static void test_pure_sine(void)
{
    printf("\n[Test] Pure 1kHz sine, amplitude 1.0\n");

    goertzel_state_t g;
    goertzel_init(&g, 1000.0f, SAMPLE_RATE, BLOCK_SIZE);

    float amplitude = 1.0f;
    for (uint32_t i = 0; i < BLOCK_SIZE; i++) {
        float sample = amplitude * sinf(2.0f * M_PI * 1000.0f * i / SAMPLE_RATE);
        goertzel_process_sample(&g, sample);
    }

    float mag, phase;
    goertzel_get_result(&g, &mag, &phase);

    check("magnitude", mag, amplitude, TOLERANCE);
}

/**
 * Test 2: Sine at different frequency — should report ~0 at target.
 */
static void test_wrong_frequency(void)
{
    printf("\n[Test] 2kHz sine, measuring at 1kHz (should be ~0)\n");

    goertzel_state_t g;
    goertzel_init(&g, 1000.0f, SAMPLE_RATE, BLOCK_SIZE);

    for (uint32_t i = 0; i < BLOCK_SIZE; i++) {
        float sample = 1.0f * sinf(2.0f * M_PI * 2000.0f * i / SAMPLE_RATE);
        goertzel_process_sample(&g, sample);
    }

    float mag, phase;
    goertzel_get_result(&g, &mag, &phase);
    (void)phase;

    check("magnitude", mag, 0.0f, TOLERANCE);
}

/**
 * Test 3: DC signal — should report 0 at 1kHz.
 */
static void test_dc_signal(void)
{
    printf("\n[Test] DC signal = 1.0, measuring at 1kHz (should be ~0)\n");

    goertzel_state_t g;
    goertzel_init(&g, 1000.0f, SAMPLE_RATE, BLOCK_SIZE);

    for (uint32_t i = 0; i < BLOCK_SIZE; i++) {
        goertzel_process_sample(&g, 1.0f);
    }

    float mag, phase;
    goertzel_get_result(&g, &mag, &phase);
    (void)phase;

    check("magnitude", mag, 0.0f, TOLERANCE);
}

/**
 * Test 4: Two-tone signal — extract correct 1kHz and 2kHz amplitudes.
 */
static void test_two_tone(void)
{
    printf("\n[Test] Two-tone: 1kHz (amp=0.8) + 2kHz (amp=0.3)\n");

    goertzel_state_t g1, g2;
    goertzel_init(&g1, 1000.0f, SAMPLE_RATE, BLOCK_SIZE);
    goertzel_init(&g2, 2000.0f, SAMPLE_RATE, BLOCK_SIZE);

    for (uint32_t i = 0; i < BLOCK_SIZE; i++) {
        float t = (float)i / SAMPLE_RATE;
        float sample = 0.8f * sinf(2.0f * M_PI * 1000.0f * t) +
                        0.3f * sinf(2.0f * M_PI * 2000.0f * t);
        goertzel_process_sample(&g1, sample);
        goertzel_process_sample(&g2, sample);
    }

    float mag1, phase1, mag2, phase2;
    goertzel_get_result(&g1, &mag1, &phase1);
    goertzel_get_result(&g2, &mag2, &phase2);
    (void)phase1;
    (void)phase2;

    check("1kHz magnitude", mag1, 0.8f, TOLERANCE);
    check("2kHz magnitude", mag2, 0.3f, TOLERANCE);
}

/**
 * Test 5: Phase detection — verify phase of sine vs cosine.
 */
static void test_phase(void)
{
    printf("\n[Test] Phase detection: sin (phase=−pi/2) vs cos (phase=0)\n");

    /* Sine wave: phase should be -pi/2 */
    goertzel_state_t g_sin;
    goertzel_init(&g_sin, 1000.0f, SAMPLE_RATE, BLOCK_SIZE);
    for (uint32_t i = 0; i < BLOCK_SIZE; i++) {
        float sample = sinf(2.0f * M_PI * 1000.0f * i / SAMPLE_RATE);
        goertzel_process_sample(&g_sin, sample);
    }
    float mag_s, phase_s;
    goertzel_get_result(&g_sin, &mag_s, &phase_s);

    /* Cosine wave: phase should be 0 */
    goertzel_state_t g_cos;
    goertzel_init(&g_cos, 1000.0f, SAMPLE_RATE, BLOCK_SIZE);
    for (uint32_t i = 0; i < BLOCK_SIZE; i++) {
        float sample = cosf(2.0f * M_PI * 1000.0f * i / SAMPLE_RATE);
        goertzel_process_sample(&g_cos, sample);
    }
    float mag_c, phase_c;
    goertzel_get_result(&g_cos, &mag_c, &phase_c);

    (void)mag_s;
    (void)mag_c;

    /* Phase difference should be ~pi/2 */
    float phase_diff = fabsf(phase_c - phase_s);
    check("phase difference (sin vs cos)", phase_diff, (float)(M_PI / 2.0), 0.05f);
}

/**
 * Test 6: Multiple blocks — reset and reuse.
 */
static void test_multi_block(void)
{
    printf("\n[Test] Multiple blocks with reset\n");

    goertzel_state_t g;
    goertzel_init(&g, 1000.0f, SAMPLE_RATE, BLOCK_SIZE);

    for (int block = 0; block < 3; block++) {
        goertzel_reset(&g);
        for (uint32_t i = 0; i < BLOCK_SIZE; i++) {
            float sample = 0.5f * sinf(2.0f * M_PI * 1000.0f * i / SAMPLE_RATE);
            goertzel_process_sample(&g, sample);
        }
        float mag, phase;
        goertzel_get_result(&g, &mag, &phase);
        (void)phase;
        check("block magnitude", mag, 0.5f, TOLERANCE);
    }
}

int main(void)
{
    printf("=== Goertzel Algorithm Unit Tests ===\n");
    printf("Sample rate: %.0f Hz, Block size: %d\n", SAMPLE_RATE, BLOCK_SIZE);

    test_pure_sine();
    test_wrong_frequency();
    test_dc_signal();
    test_two_tone();
    test_phase();
    test_multi_block();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
