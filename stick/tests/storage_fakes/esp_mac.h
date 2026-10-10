#pragma once
#include <cstdint>
#include <cstring>
inline int esp_efuse_mac_get_default(uint8_t *out)
{
  const uint8_t mac[6] = {2, 1, 2, 3, 4, 5};
  memcpy(out, mac, sizeof(mac));
  return 0;
}
