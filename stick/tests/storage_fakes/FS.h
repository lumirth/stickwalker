#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fs {
inline std::map<std::string, std::vector<uint8_t> > files;
inline long writeBudget = -1;
inline bool failRename = false;

class File {
  std::string path;
  size_t cursor = 0;
  bool valid = false;

public:
  File() = default;
  File(const char *name, const char *mode) : path(name)
  {
    if (*mode == 'w')
      files[path].clear();
    if (*mode == 'a')
      cursor = files[path].size();
    valid = files.count(path) != 0;
  }
  explicit operator bool() const
  {
    return valid;
  }
  size_t size() const
  {
    return valid ? files.at(path).size() : 0;
  }
  size_t position() const
  {
    return cursor;
  }
  bool seek(size_t offset)
  {
    cursor = offset;
    return valid && offset <= size();
  }
  size_t read(uint8_t *out, size_t count)
  {
    if (!valid || cursor > size())
      return 0;
    count = std::min(count, size() - cursor);
    if (count) memcpy(out, files.at(path).data() + cursor, count);
    cursor += count;
    return count;
  }
  size_t write(const uint8_t *in, size_t count)
  {
    if (!valid)
      return 0;
    if (writeBudget >= 0) {
      count = std::min(count, size_t(writeBudget));
      writeBudget -= count;
    }
    auto &bytes = files.at(path);
    bytes.resize(std::max(bytes.size(), cursor + count));
    if (count) memcpy(bytes.data() + cursor, in, count);
    cursor += count;
    return count;
  }
  void flush()
  {
  }
  void close()
  {
    valid = false;
  }
};
} // namespace fs

struct FakeLittleFS {
  bool begin(bool)
  {
    return true;
  }
  fs::File open(const char *path, const char *mode)
  {
    return fs::File(path, mode);
  }
  bool exists(const char *path)
  {
    return fs::files.count(path) != 0;
  }
  bool remove(const char *path)
  {
    return fs::files.erase(path) != 0;
  }
  bool rename(const char *from, const char *to)
  {
    if (fs::failRename || !exists(from))
      return false;
    fs::files[to] = fs::files.at(from);
    fs::files.erase(from);
    return true;
  }
};
inline FakeLittleFS LittleFS;
