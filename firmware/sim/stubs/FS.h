// Host stand-in for Arduino-ESP32's <FS.h> (firmware/sim). sd_logger.h names the `File` type in
// two declarations the device page never calls; this gives the compiler a type to attach them to.
#pragma once

namespace fs {
class File {
 public:
  explicit operator bool() const { return false; }
  void close() {}
};
}  // namespace fs
using fs::File;
