#ifndef SPEN_SERVO_H
#define SPEN_SERVO_H
#include <math.h>

/* Converge-to-target servo: steer an estimated cursor (game px) toward the
   absolute pen target, emitting a delta in SNES mouse units (clamped to +-127).
   Kp=1.0 -> zero-lag tracking; gain (=S_assumed) calibrates units->px. */
typedef struct {
    double kp;        /* responsiveness; 1.0 = move full error each frame */
    double gain;      /* S_assumed: mouse-units -> game pixels */
    int    deadzone;  /* px of error ignored (anti-jitter / rest stability) */
    int    clamp;     /* max units per poll; SNES HW limit = 127 */
} spen_servo_params;

/* Returns the per-axis step (mouse units) to emit and advances *est. */
static inline int spen_servo_step(double target, double *est, const spen_servo_params *p)
{
    double error = target - *est;
    if (error <= p->deadzone && error >= -p->deadzone)
        return 0;
    double s = p->kp * error / p->gain;
    int step = (int)floor(s + 0.5);            /* match JS Math.round */
    if (step >  p->clamp) step =  p->clamp;
    if (step < -p->clamp) step = -p->clamp;
    if (step == 0) step = (error > 0) ? 1 : -1; /* guarantee progress outside deadzone */
    *est += (double)step * p->gain;
    return step;
}
#endif /* SPEN_SERVO_H */
