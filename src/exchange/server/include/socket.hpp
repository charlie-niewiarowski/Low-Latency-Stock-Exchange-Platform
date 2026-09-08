//
// Created by cniew on 5/11/26.
//

#ifndef SOCKET_H
#define SOCKET_H

#include <unistd.h>
#include <sys/epoll.h>

// socket RAII wrapper
class EpollSocket {
public:
    explicit EpollSocket(int fd, int epoll_fd) : fd_(fd), epoll_fd_(epoll_fd) {}
    EpollSocket() : fd_(-1), epoll_fd_(-1) {}

    ~EpollSocket() {
        if (fd_ != -1) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
            close(fd_);
        }
    }

    EpollSocket(const EpollSocket&) = delete;
    EpollSocket& operator=(const EpollSocket&) = delete;

    EpollSocket(EpollSocket&& other) noexcept : fd_(other.fd_), epoll_fd_(other.epoll_fd_) {
        other.fd_ = -1;
        other.epoll_fd_ = -1;
    }

    EpollSocket& operator=(EpollSocket&& other) noexcept {
        if (this != &other) {
            if (fd_ != -1) {
                epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
                ::close(fd_);
            }
            fd_ = other.fd_;
            epoll_fd_ = other.epoll_fd_;
            other.fd_ = -1;
            other.epoll_fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    int release() { int f = fd_; fd_ = -1; return f; }
private:
    int fd_;
    int epoll_fd_;
};

#endif //SOCKET_H
