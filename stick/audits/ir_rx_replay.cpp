#include "../ir_rx_core.h"

#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  FILE *file = std::fopen(argv[1], "rb");
  if (!file) return 2;
  std::vector<uint8_t> bins;
  for (int byte = std::fgetc(file); byte != EOF; byte = std::fgetc(file))
    bins.push_back(uint8_t(byte));
  std::fclose(file);
  pw_stick::WireBurst frame;
  if (!pw_stick::decode_wire_burst(bins.data(), bins.size(), frame)) return 1;
  std::printf("%u %u %u ", frame.length, frame.starts, frame.stops);
  for (unsigned i = 0; i < frame.length; ++i)
    std::printf("%02x", frame.bytes[i]);
  std::putchar('\n');
  return 0;
}
