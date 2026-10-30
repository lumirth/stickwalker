#include "../ir_rx_core.h"

#include <cstdio>

// The shared-library entry point lets the workflow replay many independently
// generated/captured streams through the same compiled decoder as firmware.
extern "C" int replay_wire(const uint8_t *samples, unsigned count,
                            uint32_t first_gate, const uint32_t *indices,
                            uint8_t *packets,
                            unsigned capacity, unsigned *rejected,
                            unsigned *gaps) {
  pw_stick::WireReceiver receiver;
  receiver.reset(first_gate);
  pw_stick::WireFrame frame;
  unsigned used = 0;
  *rejected = *gaps = 0;
  for (unsigned i = 0; i < count; ++i) {
    const auto event = receiver.feed(samples[i], indices ? indices[i] : first_gate + i, frame);
    *rejected += event.rejected;
    *gaps += event.gap;
    if (!event.frame) continue;
    if (used + frame.length + 1 > capacity) return -1;
    packets[used++] = frame.length;
    for (unsigned j = 0; j < frame.length; ++j)
      packets[used++] = frame.bytes[j];
  }
  return int(used);
}

#ifndef PW_IR_REPLAY_LIBRARY
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  FILE *file = std::fopen(argv[1], "rb");
  if (!file) return 2;
  pw_stick::WireReceiver receiver;
  pw_stick::WireFrame frame;
  unsigned count = 0, accepted = 0, rejected = 0, gaps = 0;
  for (int sample = std::fgetc(file); sample != EOF; sample = std::fgetc(file)) {
    const auto event = receiver.feed(uint8_t(sample), count++, frame);
    rejected += event.rejected;
    gaps += event.gap;
    if (!event.frame) continue;
    ++accepted;
    std::printf("first=%u last=%u contrast=%u bytes=", frame.first_gate,
                frame.last_gate, unsigned(frame.minimum_contrast));
    for (unsigned i = 0; i < frame.length; ++i)
      std::printf("%02x", frame.bytes[i]);
    std::putchar('\n');
  }
  std::fclose(file);
  std::fprintf(stderr, "samples=%u frames=%u rejected=%u gaps=%u\n", count,
                accepted, rejected, gaps);
  return accepted ? 0 : 1;
}
#endif
