#pragma once
#include <cmath>
#include <cstdint>

// Sampled once per completed USB transfer, independent of the UART sender.
// MouseReport is the unchanged six-byte USB report declared by the sketch.
struct CircleMotion {
  bool active = false;
  uint32_t step = 0, steps = 0, period = 0;
  int radius = 0, x = 0, y = 0;
  uint8_t buttons = 0;

  void start(int r, uint32_t p, uint32_t turns, uint8_t held = 0) {
    radius = x = r; y = 0;
    period = p; steps = p * turns; step = 0;
    buttons = held; active = true;
  }

  MouseReport next() {
    // Include a final release report, even in drawing mode.
    if (!active || step == steps) {
      active = false;
      return {0, 0, 0, 0};
    }
    double angle = 6.283185307179586 * (++step) / period;
    int nx = std::lround(radius * std::cos(angle));
    int ny = std::lround(radius * std::sin(angle));
    MouseReport report = {buttons, 0, int16_t(nx - x), int16_t(ny - y)};
    x = nx; y = ny;
    return report;
  }
};
