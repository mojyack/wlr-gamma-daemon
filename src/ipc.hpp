#pragma once
#include <bit>

namespace ipc {
auto create(const char* name) -> int;
auto read(const int fd) -> void;
} // namespace ipc
