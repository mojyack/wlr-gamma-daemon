#include <array>

#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "assert.hpp"
#include "ipc.hpp"

namespace ipc {
auto create(const char* name) -> int {
    const auto fd = inotify_init();
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    inotify_add_watch(fd, name, IN_CLOSE_WRITE);
    return fd;
}

auto read(const int fd) -> void {
    constexpr auto buflen = sizeof(inotify_event);

    auto buf = std::array<std::byte, buflen>();
loop:
    const auto len = ::read(fd, buf.data(), buflen);
    if(len == -1 && errno == EAGAIN) {
        return;
    }
    DYN_ASSERT(len == buflen);
    goto loop;

    return;
}
} // namespace ipc
