#include "ir_rx_core.h"

#include <limits.h>
#include <string.h>

namespace pw_stick {
namespace {

constexpr unsigned kCells = kMaxWireBytes * 10;
constexpr unsigned kMaxStrongGates = 4096;

struct Geometry {
  int origin_twice = 0;
  int inverse_q15 = 0;
  int step_10000 = 0;
  unsigned bytes = 0;
  uint8_t occupied[kCells] = {};
};

int nearest_cell(unsigned gate, const Geometry &geometry) {
  const int32_t position_q16 =
      (int32_t(gate) * 2 - geometry.origin_twice) * geometry.inverse_q15;
  return (position_q16 + (1 << 15)) >> 16;
}

void map_cells(const uint16_t *strong, unsigned count, Geometry &geometry,
               uint64_t &phase_error) {
  memset(geometry.occupied, 0, sizeof(geometry.occupied));
  phase_error = 0;
  for (unsigned i = 0; i < count; ++i) {
    const int32_t position_q16 =
        (int32_t(strong[i]) * 2 - geometry.origin_twice) *
        geometry.inverse_q15;
    const int cell = (position_q16 + (1 << 15)) >> 16;
    if (cell >= 0 && cell < int(geometry.bytes * 10))
      geometry.occupied[cell] = 1;
    const int64_t offset = int64_t(position_q16) - int64_t(cell) * 65536;
    phase_error += uint64_t(offset * offset);
  }
}

unsigned framing_errors(const Geometry &geometry) {
  unsigned bad = 0;
  for (unsigned byte = 0; byte < geometry.bytes; ++byte) {
    bad += !geometry.occupied[byte * 10];
    bad += geometry.occupied[byte * 10 + 9];
  }
  return bad;
}

bool fit_geometry(const uint16_t *strong, unsigned count, Geometry &best) {
  const unsigned span = strong[count - 1] - strong[0];
  unsigned bytes = unsigned(double(span) / 30.04) + 1;
  if (!bytes || bytes > kMaxWireBytes) return false;

  for (unsigned length_trial = 0; length_trial < 2; ++length_trial) {
    unsigned best_bad = UINT_MAX;
    uint64_t best_phase = UINT64_MAX;
    for (int step = 30000; step <= 30080; step += 5) {
      for (int half = -10; half <= 8; ++half) {
        // This decoder is called only from the foreground IR transport. Keep
        // the large geometry workspace out of Arduino's 8 KiB loop stack.
        static Geometry candidate;
        candidate.bytes = bytes;
        candidate.origin_twice = int(strong[0]) * 2 + half;
        candidate.step_10000 = step;
        candidate.inverse_q15 =
            int(((1u << 15) * 10000u + unsigned(step / 2)) / unsigned(step));
        // A missing first optical pulse can leave the first data-zero pulse
        // as the first strong observation. Let UART framing across the whole
        // burst decide between cell 0 and cell 1, without using payload bytes.
        const int opening_cell = nearest_cell(strong[0], candidate);
        if (opening_cell < 0 || opening_cell > 1) continue;
        uint64_t phase = 0;
        map_cells(strong, count, candidate, phase);
        const unsigned bad = framing_errors(candidate);
        if (bad < best_bad || (bad == best_bad && phase < best_phase)) {
          best = candidate;
          best_bad = bad;
          best_phase = phase;
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

bool decode_wire_burst(const uint8_t *bins, size_t gate_count, WireBurst &out,
                       uint8_t pulse_cutoff) {
  out = WireBurst{};
  if (!bins || !gate_count || gate_count > kMaxBurstGates) return false;
  // The previous automatic buffers made this function's machine stack frame
  // 11,088 bytes, overflowing the 8,192-byte Arduino loop task.
  static uint16_t strong[kMaxStrongGates];
  unsigned strong_count = 0;
  for (size_t gate = 0; gate < gate_count; ++gate) {
    if (bins[gate] >= pulse_cutoff) continue;
    if (strong_count == kMaxStrongGates) return false;
    strong[strong_count++] = uint16_t(gate);
  }
  if (!strong_count) return false;

  static Geometry geometry;
  if (!fit_geometry(strong, strong_count, geometry)) return false;
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
  return true;
}

}  // namespace pw_stick
