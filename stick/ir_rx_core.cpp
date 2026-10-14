#include "ir_rx_core.h"

#include <limits.h>
#include <string.h>

namespace pw_stick {
namespace {

constexpr unsigned kCells = kMaxWireBytes * 10;
constexpr unsigned kMaxStrongGates = 4096;
int learned_step_10000 = 0;

struct Geometry {
  int origin_twice = 0;
  int inverse_q15 = 0;
  int step_10000 = 0;
  unsigned bytes = 0;
  uint8_t fit_pass = 0;
  uint8_t occupied[kCells] = {};
};

int nearest_cell(unsigned gate, const Geometry &geometry) {
  const int32_t position_q16 =
      (int32_t(gate) * 2 - geometry.origin_twice) * geometry.inverse_q15;
  return (position_q16 + (1 << 15)) >> 16;
}

void map_cells(const uint16_t *strong, unsigned count, Geometry &geometry) {
  memset(geometry.occupied, 0, sizeof(geometry.occupied));
  for (unsigned i = 0; i < count; ++i) {
    const int32_t position_q16 =
        (int32_t(strong[i]) * 2 - geometry.origin_twice) *
        geometry.inverse_q15;
    const int cell = (position_q16 + (1 << 15)) >> 16;
    if (cell >= 0 && cell < int(geometry.bytes * 10))
      geometry.occupied[cell] = 1;
  }
}

bool score_geometry(const uint16_t *strong, unsigned count,
                     const Geometry &geometry, unsigned best_bad,
                     uint64_t best_phase, bool earlier, unsigned &bad,
                     uint64_t &phase) {
  // Sorted gates map monotonically to cells. Count each occupied start/stop
  // once, and reject as soon as stop errors alone exceed the current best.
  // With perfect framing already found, nonnegative phase-error terms also
  // let us reject a candidate once its partial sum cannot beat the winner.
  // Neither bound changes the framing-first, phase-second search ordering.
  unsigned starts = 0, stops = 0;
  int previous = -1;
  phase = 0;
  for (unsigned i = 0; i < count; ++i) {
    const int32_t position_q16 =
        (int32_t(strong[i]) * 2 - geometry.origin_twice) * geometry.inverse_q15;
    const int cell = (position_q16 + (1 << 15)) >> 16;
    const int32_t offset = position_q16 - cell * 65536;
    phase += uint32_t(offset * offset);
    if (cell >= 0 && cell < int(geometry.bytes * 10) && cell != previous) {
      previous = cell;
      const unsigned bit = unsigned(cell) % 10;
      starts += bit == 0;
      stops += bit == 9;
    }
    // A later gate cannot fill a missing start in this already-observed prefix.
    const unsigned prefix = cell < 0 ? 0 :
        (unsigned(cell) / 10 + 1 < geometry.bytes ?
         unsigned(cell) / 10 + 1 : geometry.bytes);
    const unsigned lower_bound = prefix - starts + stops;
    if (lower_bound > best_bad ||
        (lower_bound == best_bad &&
         (phase > best_phase || (phase == best_phase && !earlier)))) return false;
  }
  bad = geometry.bytes - starts + stops;
  return bad <= best_bad;
}

bool fit_geometry(const uint16_t *strong, unsigned count, size_t gate_count,
                  Geometry &best) {
  const unsigned span = strong[count - 1] - strong[0];
  unsigned bytes = unsigned(double(span) / 30.04) + 1;
  if (!bytes || bytes > kMaxWireBytes) return false;

  for (unsigned length_trial = 0; length_trial < 2; ++length_trial) {
    unsigned best_bad = UINT_MAX;
    uint64_t best_phase = UINT64_MAX;
    // Try the previous long burst's baud estimate first, or estimate it from
    // this burst's gate count. Search every start phase because the first
    // observed pulse may be a data bit.
    const bool short_frame = bytes <= 16;
    const int estimated_step =
        int((gate_count * 10000u + bytes * 5u) / (bytes * 10u));
    const unsigned passes = short_frame ? 1u : (learned_step_10000 ? 3u : 2u);
    for (unsigned pass = 0; pass < passes; ++pass) {
      if (pass && best_bad <= 1) break;
      const bool full_search = short_frame || pass == passes - 1;
      const int anchor = pass == 0 && learned_step_10000 ?
          learned_step_10000 : estimated_step;
      const int radius = pass == 0 && learned_step_10000 ? 25 : 35;
      const int first_step = full_search ? 29700 :
          (anchor - radius < 29700 ? 29700 : anchor - radius);
      const int last_step = full_search ? 30120 :
          (anchor + radius > 30120 ? 30120 : anchor + radius);
      if (first_step > last_step) continue;
      const int steps = (last_step - first_step) / 5 + 1;
      int center = (anchor - first_step + 2) / 5;
      if (center < 0) center = 0;
      if (center >= steps) center = steps - 1;
      // Visit the likely baud first so the same exact search gets a useful
      // rejection bound early. Explicit ties preserve its original ordering.
      for (int trial = 0; trial < steps * 2; ++trial) {
        const int index = center + ((trial + 1) / 2) * (trial & 1 ? -1 : 1);
        if (index < 0 || index >= steps) continue;
        const int step = first_step + index * 5;
        for (int half = -10; half <= 8; ++half) {
          // Keep the geometry workspace in static storage to fit the
          // foreground task's 8 KiB stack.
          static Geometry candidate;
          candidate.bytes = bytes;
          candidate.origin_twice = int(strong[0]) * 2 + half;
          candidate.step_10000 = step;
          candidate.fit_pass = uint8_t(pass);
          candidate.inverse_q15 = int(((1u << 15) * 10000u +
                                       unsigned(step / 2)) / unsigned(step));
          // If the first optical pulse is missed, UART framing distinguishes
          // a start bit from the first data-zero pulse.
          const int opening_cell = nearest_cell(strong[0], candidate);
          if (opening_cell < 0 || opening_cell > 1) continue;
          unsigned bad;
          uint64_t phase;
          const bool earlier = candidate.fit_pass < best.fit_pass ||
              (candidate.fit_pass == best.fit_pass && step < best.step_10000);
          if (!score_geometry(strong, count, candidate, best_bad, best_phase,
                               earlier, bad, phase)) continue;
          if (bad < best_bad || (bad == best_bad &&
              (phase < best_phase || (phase == best_phase && earlier)))) {
            best = candidate;
            best_bad = bad;
            best_phase = phase;
          }
        }
      }
    }
    const int last_cell = nearest_cell(strong[count - 1], best);
    if (length_trial == 0 && bytes < kMaxWireBytes &&
        last_cell >= int(bytes * 10) && last_cell < int((bytes + 1) * 10)) {
      ++bytes;
      continue;
    }
    break;
  }
  return true;
}

void remove_recovery_tails(const uint8_t *bins, const uint16_t *strong,
                           unsigned count, Geometry &geometry,
                           WireBurst &out) {
  static uint8_t observations[kCells];
  memset(observations, 0, sizeof(observations));
  for (unsigned i = 0; i < count; ++i) {
    const int cell = nearest_cell(strong[i], geometry);
    if (cell >= 0 && cell < int(geometry.bytes * 10) &&
        observations[cell] != UINT8_MAX)
      ++observations[cell];
  }
  for (unsigned i = 1; i < count; ++i) {
    if (strong[i] != strong[i - 1] + 1) continue;
    const int left = nearest_cell(strong[i - 1], geometry);
    const int right = nearest_cell(strong[i], geometry);
    if (left < 0 || right >= int(geometry.bytes * 10) || right != left + 1)
      continue;
    if (bins[strong[i - 1]] == bins[strong[i]]) continue;
    const int weaker = bins[strong[i - 1]] > bins[strong[i]] ? left : right;
    if (observations[weaker] == 1 && geometry.occupied[weaker]) {
      geometry.occupied[weaker] = 0;
      ++out.recovery_tails;
    }
  }
}

bool weak_pair_in_cell(const uint8_t *bins, size_t gates, unsigned cell,
                       const Geometry &geometry, uint8_t weak_cutoff) {
  const int center = (geometry.origin_twice * 10000 +
                      int(cell) * geometry.step_10000 * 2 + 10000) /
                     20000;
  for (int gate = center - 3; gate <= center + 2; ++gate) {
    if (gate < 0 || size_t(gate + 1) >= gates) continue;
    if (nearest_cell(unsigned(gate), geometry) == int(cell) &&
        nearest_cell(unsigned(gate + 1), geometry) == int(cell) &&
        bins[gate] < weak_cutoff && bins[gate + 1] < weak_cutoff)
      return true;
  }
  return false;
}

}  // namespace

void reset_rx_timing() { learned_step_10000 = 0; }

bool decode_wire_burst(const uint8_t *bins, size_t gate_count, WireBurst &out,
                       uint8_t pulse_cutoff) {
  out = WireBurst{};
  if (!bins || !gate_count || gate_count > kMaxBurstGates) return false;
  // Foreground-only decoding uses static buffers to fit the 8 KiB loop stack.
  static uint16_t strong[kMaxStrongGates];
  unsigned strong_count = 0;
  for (size_t gate = 0; gate < gate_count; ++gate) {
    if (bins[gate] >= pulse_cutoff) continue;
    if (strong_count == kMaxStrongGates) return false;
    strong[strong_count++] = uint16_t(gate);
  }
  if (!strong_count) return false;

  static Geometry geometry;
  if (!fit_geometry(strong, strong_count, gate_count, geometry)) return false;
  map_cells(strong, strong_count, geometry);
  remove_recovery_tails(bins, strong, strong_count, geometry, out);

  for (unsigned byte = 0; byte < geometry.bytes; ++byte) {
    const unsigned base = byte * 10;
    uint8_t value = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
      const unsigned cell = base + bit + 1;
      bool pulse = geometry.occupied[cell];
      if (!pulse && weak_pair_in_cell(bins, gate_count, cell, geometry,
                                     uint8_t(pulse_cutoff + 2))) {
        pulse = true;
        ++out.weak_pairs;
      }
      if (!pulse) value |= uint8_t(1u << bit);
    }
    out.bytes[byte] = value;
    out.starts += geometry.occupied[base] != 0;
    out.stops += geometry.occupied[base + 9] == 0;
  }
  out.length = uint8_t(geometry.bytes);
  out.baud_step_10000 = uint16_t(geometry.step_10000);
  out.origin_twice = int16_t(geometry.origin_twice);
  out.fit_pass = geometry.fit_pass;
  if (out.length >= 32 && out.credible_uart())
    learned_step_10000 = geometry.step_10000;
  return true;
}

}  // namespace pw_stick
