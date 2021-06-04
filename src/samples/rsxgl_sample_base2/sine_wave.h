#include <math.h>
#ifndef COMPUTE_SINE_WAVE
#define COMPUTE_SINE_WAVE

#if defined(__cplusplus)
extern "C" {
#endif

  /*
    A - amplitude
    D - center amplitude
    omega - frequency
   */
struct sine_wave_t {
  float A, D, omega;
};

float compute_sine_wave(const struct sine_wave_t *,float);

#if defined(__cplusplus)
}
#endif

#endif
