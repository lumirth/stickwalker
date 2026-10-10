#pragma once
#include <cstddef>
#include <cstring>
#define ESP_OK 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 2
struct esp_partition_t {
  size_t size;
};
inline const esp_partition_t *esp_partition_find_first(int, int, const char *)
{
  static esp_partition_t partition = {65536};
  return &partition;
}
inline int esp_partition_read(const esp_partition_t *, size_t, void *out,
                              size_t size)
{
  memset(out, 0xff, size);
  return ESP_OK;
}
