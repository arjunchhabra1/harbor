#include "harbor/wakeup.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace harbor {

Wakeup::Wakeup() {
    int fds[2];
    if (::pipe(fds) != 0) {
        throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
    }
    read_ = Fd(fds[0]);
    write_ = Fd(fds[1]);
    setNonBlocking(read_.get());
    setNonBlocking(write_.get());
}

void Wakeup::signal() {
    char byte = 1;
    // A full pipe already holds a pending wakeup, so EAGAIN is fine to ignore.
    [[maybe_unused]] ssize_t n = ::write(write_.get(), &byte, 1);
}

void Wakeup::drain() {
    char buf[64];
    while (::read(read_.get(), buf, sizeof buf) > 0) {
    }
}

}  // namespace harbor
