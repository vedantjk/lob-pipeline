#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "frontend.h"

constexpr int BATCH = 64;   // packets drained per recvmmsg
constexpr int MAXEV = 4;    // one socket, so readiness list is tiny

int main(int argc, char** argv)
{
    bool emit = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--emit") emit = true;

    mlockall(MCL_CURRENT | MCL_FUTURE);
    if (!pin_to_core(6)) std::cerr << "warning: could not pin to core 6\n";

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    const int rcvbuf = 128 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
#ifdef QTIME
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MOLD_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { perror("bind"); return 1; }

    const int epfd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;                 // level-triggered: re-fires while data remains
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) { perror("epoll_ctl"); return 1; }
    epoll_event events[MAXEV];

    static mmsghdr msgs[BATCH];
    static iovec iovs[BATCH];
    static uint8_t bufs[BATCH][MOLD_MAX_PACKET];
    std::memset(bufs, 0, sizeof(bufs));
#ifdef QTIME
    static char ctrl[BATCH][CMSG_SPACE(sizeof(timespec))];
    uint64_t rx_ns[BATCH];
#endif
    for (int i = 0; i < BATCH; ++i)
    {
        iovs[i].iov_base = bufs[i];
        iovs[i].iov_len = MOLD_MAX_PACKET;
        std::memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
#ifdef QTIME
        msgs[i].msg_hdr.msg_control = ctrl[i];
#endif
    }

    Frontend fe;
    if (emit) fe.open_tob("../results/rung3_AAPL.tob");

    const uint8_t* ptrs[BATCH];
    unsigned lens[BATCH];
    bool running = true;
    while (running)
    {
        // Syscall 1: readiness. Blocking wait — this is the epoll tax over Rung 2,
        // which folds readiness+recv into one recvmmsg.
#ifdef MEASURE
        const uint64_t e0 = now_cycles();
#endif
        const int nev = epoll_wait(epfd, events, MAXEV, 1000);   // 1 s idle timeout
#ifdef MEASURE
        const uint32_t epoll_cyc = static_cast<uint32_t>(now_cycles() - e0);
#else
        const uint32_t epoll_cyc = 0;
#endif
        if (nev < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }
        if (nev == 0) { if (fe.msg_index > 0) break; else continue; }   // idle backstop

        // Syscall 2: data. DONTWAIT — level-triggered epoll guarantees >=1 packet.
#ifdef QTIME
        for (int i = 0; i < BATCH; ++i) msgs[i].msg_hdr.msg_controllen = sizeof(ctrl[i]);
#endif
#ifdef MEASURE
        const uint64_t r0 = now_cycles();
#endif
        const int n = recvmmsg(fd, msgs, BATCH, MSG_DONTWAIT, nullptr);
#ifdef MEASURE
        const uint32_t recv_cyc = static_cast<uint32_t>(now_cycles() - r0);
#else
        const uint32_t recv_cyc = 0;
#endif
        if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) continue; perror("recvmmsg"); break; }

        for (int i = 0; i < n; ++i) { ptrs[i] = bufs[i]; lens[i] = msgs[i].msg_len; }
#ifdef QTIME
        for (int i = 0; i < n; ++i) rx_ns[i] = frontend_rx_ns(&msgs[i].msg_hdr);
        running = fe.process_batch(ptrs, lens, n, recv_cyc, epoll_cyc, 0, rx_ns);
#else
        running = fe.process_batch(ptrs, lens, n, recv_cyc, epoll_cyc, 0);
#endif
    }

    fe.finalize("../results/rung3_latency.csv", "rung3-epoll", "epoll_wait", true);
    return 0;
}
