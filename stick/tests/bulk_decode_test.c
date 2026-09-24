#include <assert.h>
#include <stdint.h>
#include <string.h>

void BulkDecode(uint8_t *packed, uint8_t *out);

int main(void) {
  /* One literal followed by an overlapping one-byte-back phrase. The H8
   * pointer wraps here; the Stick port must explicitly address behind out. */
  uint8_t packed[] = {0x10, 5, 0, 0, 0x40, 'A', 0x10, 0};
  uint8_t decoded[5] = {};
  BulkDecode(packed, decoded);
  assert(memcmp(decoded, "AAAAA", sizeof(decoded)) == 0);
  return 0;
}
