

#ifndef EPOLL_H
#define EPOLL_H

#include <stdint.h>

enum EPOLL_EVENTS {
    EPOLLIN      = 0x0001,
    EPOLLOUT     = 0x0002,
    EPOLLRDHUP   = 0x0004,
    EPOLLPRI     = 0x0008,
    EPOLLERR     = 0x0010,
    EPOLLHUP     = 0x0020,
    EPOLLET      = 0x0040,
    EPOLLONESHOT = 0x0080
};

enum EPOLL_OPCODES {
    EPOLL_CTL_ADD,
    EPOLL_CTL_DEL,
    EPOLL_CTL_MOD
};

typedef union epoll_data {
    void* ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t events;
    epoll_data_t data;
};

#ifdef __cplusplus
extern "C" {
#endif

int epoll_startup();
int epoll_create(int size);
int epoll_ctl(int epfd, int opcode, int fd, struct epoll_event* event);
int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout);
int epoll_close(int epfd);
void epoll_cleanup();

#ifdef __cplusplus
}
#endif

#endif /* EPOLL_H */
