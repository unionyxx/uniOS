#include "math.h"

#include <stdint.h>
#include <string.h>

// Freestanding math for uniOS userspace. No libm is linked, so these are
// self-contained implementations: bit-level sqrt seeding plus Newton
// refinement, and range-reduced polynomial sin/cos. Accuracy targets UI and
// audio use, not numerical libraries.

double fabs(double x)
{
    return x < 0.0 ? -x : x;
}

float fabsf(float x)
{
    return x < 0.0f ? -x : x;
}

double fmod(double x, double y)
{
    if (y == 0.0)
        return 0.0;
    double q = x / y;
    double qi = (q >= 0.0) ? (double)(long long)q : -(double)(long long)(-q);
    return x - qi * y;
}

float fmodf(float x, float y)
{
    return (float)fmod((double)x, (double)y);
}

double sqrt(double x)
{
    if (x <= 0.0)
        return 0.0;

    // Seed with a bit-manipulation estimate (halved exponent + bias and
    // mantissa correction), then refine with Newton iterations.
    double guess;
    uint64_t bits;
    memcpy(&bits, &x, sizeof(bits));
    bits = (bits >> 1) + 0x1FF78265F0FD5D00ull;
    memcpy(&guess, &bits, sizeof(guess));
    if (!(guess > 0.0))
        guess = x;

    for (int i = 0; i < 6; i++)
        guess = 0.5 * (guess + x / guess);
    return guess;
}

float sqrtf(float x)
{
    if (x <= 0.0f)
        return 0.0f;
    float guess;
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    bits = (bits >> 1) + 0x1FBC132Fu;
    memcpy(&guess, &bits, sizeof(guess));
    if (!(guess > 0.0f))
        guess = x;
    for (int i = 0; i < 4; i++)
        guess = 0.5f * (guess + x / guess);
    return guess;
}

static const double k_pi = 3.14159265358979323846;
static const double k_two_pi = 6.28318530717958647692;

// Reduce x into [-pi, pi] and evaluate the sine Taylor series.
static double sin_reduced(double x)
{
    double x2 = x * x;
    double term = x;
    double sum = x;
    for (int n = 1; n <= 12; n++) {
        term *= -x2 / ((double)(2 * n) * (double)(2 * n + 1));
        sum += term;
    }
    return sum;
}

double sin(double x)
{
    if (x != x)
        return 0.0; // NaN guard
    x = fmod(x, k_two_pi);
    if (x > k_pi)
        x -= k_two_pi;
    else if (x < -k_pi)
        x += k_two_pi;
    return sin_reduced(x);
}

double cos(double x)
{
    return sin(x + k_pi * 0.5);
}

double tan(double x)
{
    double c = cos(x);
    if (c == 0.0)
        return 0.0;
    return sin(x) / c;
}

float sinf(float x)
{
    return (float)sin((double)x);
}

float cosf(float x)
{
    return (float)cos((double)x);
}

float tanf(float x)
{
    return (float)tan((double)x);
}
