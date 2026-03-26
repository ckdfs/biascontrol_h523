/**
 * Host-side unit test for the PI controller.
 *
 * Build: gcc -o test_pid test_pid.c ../control/src/ctrl_pid.c -lm
 * Run:   ./test_pid
 */

#include <stdio.h>
#include <math.h>
#include "ctrl_pid.h"

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
 * Test 1: Proportional response only (Ki=0).
 */
static void test_proportional(void)
{
    printf("\n[Test] Proportional only (Kp=2, Ki=0)\n");

    pid_state_t pid;
    pid_init(&pid, 2.0f, 0.0f, 0.01f, -10.0f, 10.0f);

    float out = pid_update(&pid, 1.0f);
    check("output for error=1.0", out, 2.0f, 0.001f);

    out = pid_update(&pid, -0.5f);
    check("output for error=-0.5", out, -1.0f, 0.001f);
}

/**
 * Test 2: Integral accumulation.
 */
static void test_integral(void)
{
    printf("\n[Test] Integral accumulation (Kp=0, Ki=10, dt=0.01)\n");

    pid_state_t pid;
    pid_init(&pid, 0.0f, 10.0f, 0.01f, -10.0f, 10.0f);

    /* 10 steps with error=1.0: integral = 10 * 1.0 * 0.01 = 0.1 per step */
    /* After 10 steps: integral = 1.0, output = Ki * integral = 10 * 1.0 = 10.0 */
    float out = 0;
    for (int i = 0; i < 10; i++) {
        out = pid_update(&pid, 1.0f);
    }
    /* integral = 10 * 0.01 * 1.0 = 0.1, output = 10 * 0.1 = 1.0 */
    check("output after 10 steps", out, 1.0f, 0.01f);
}

/**
 * Test 3: Output clamping.
 */
static void test_clamping(void)
{
    printf("\n[Test] Output clamping (max=5.0)\n");

    pid_state_t pid;
    pid_init(&pid, 10.0f, 0.0f, 0.01f, -5.0f, 5.0f);

    float out = pid_update(&pid, 1.0f);  /* Would be 10.0 without clamp */
    check("clamped output", out, 5.0f, 0.001f);

    out = pid_update(&pid, -1.0f);  /* Would be -10.0 without clamp */
    check("clamped negative", out, -5.0f, 0.001f);
}

/**
 * Test 4: Anti-windup — integral should not blow up.
 */
static void test_antiwindup(void)
{
    printf("\n[Test] Anti-windup\n");

    pid_state_t pid;
    pid_init(&pid, 0.0f, 1.0f, 0.01f, -5.0f, 5.0f);

    /* Drive with large error for many steps — integral should clamp at 5.0 */
    float out = 0;
    for (int i = 0; i < 10000; i++) {
        out = pid_update(&pid, 10.0f);
    }
    check("saturated output", out, 5.0f, 0.001f);

    /* Reverse — with anti-windup, integral was clamped at 5.0 (not 1000).
     * So recovery should happen within a reasonable number of steps. */
    for (int i = 0; i < 200; i++) {
        out = pid_update(&pid, -10.0f);
    }
    /* After 200 steps: integral decreases by 10*0.01=0.1 per step.
     * From 5.0, after 100 steps → 0.0, after 200 steps → clamped at -5.0.
     * Output = 1.0 * (-5.0) = -5.0 */
    check("recovered output", out, -5.0f, 0.001f);
}

/**
 * Test 5: Reset clears integral.
 */
static void test_reset(void)
{
    printf("\n[Test] Reset clears integral\n");

    pid_state_t pid;
    pid_init(&pid, 1.0f, 10.0f, 0.01f, -10.0f, 10.0f);

    /* Accumulate some integral */
    for (int i = 0; i < 100; i++) {
        pid_update(&pid, 1.0f);
    }

    pid_reset(&pid);
    float out = pid_update(&pid, 0.0f);
    check("output after reset with zero error", out, 0.0f, 0.001f);
}

int main(void)
{
    printf("=== PI Controller Unit Tests ===\n");

    test_proportional();
    test_integral();
    test_clamping();
    test_antiwindup();
    test_reset();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
