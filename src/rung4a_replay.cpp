#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "frontend.h"

constexpr int BATCH = 64;

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
    const int ts_on = 1;
    setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &ts_on, sizeof(ts_on));
#endif

    // Kernel-side busy-poll hints. On loopback there's no NIC to poll, so the
    // real mechanism is the userspace spin below; set these for realism/parity.
    const int busy_us = 50, prefer = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us)) < 0)
        std::cerr << "warning: SO_BUSY_POLL not set (needs privilege/sysctl)\n";
    setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &prefer, sizeof(prefer));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MOLD_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { perror("bind"); return 1; }

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
    if (emit) fe.open_tob("../results/rung4a_AAPL.tob");

    const uint8_t* ptrs[BATCH];
    unsigned lens[BATCH];
    bool running = true;
    while (running)
    {
        // Spin on non-blocking recv — no epoll_wait, no scheduler park. Each empty
        // call (EAGAIN) is one spin iteration; aux1 records how many before data.
        uint32_t spins = 0, recv_cyc = 0;
        int n = 0;
        timespec ts0{}; clock_gettime(CLOCK_MONOTONIC, &ts0);
        const uint64_t spin_start = static_cast<uint64_t>(ts0.tv_sec) * NS_PER_SEC + ts0.tv_nsec;
#ifdef QTIME
        for (int i = 0; i < BATCH; ++i) msgs[i].msg_hdr.msg_controllen = sizeof(ctrl[i]);
#endif
        for (;;)
        {
#ifdef MEASURE
            const uint64_t r0 = now_cycles();
#endif
            n = recvmmsg(fd, msgs, BATCH, MSG_DONTWAIT, nullptr);
#ifdef MEASURE
            const uint32_t c = static_cast<uint32_t>(now_cycles() - r0);
#endif
            if (n > 0)
            {
#ifdef MEASURE
                recv_cyc = c;
#endif
                break;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                ++spins;
                // Idle backstop: check the clock every 65536 empty spins.
                if ((spins & 0xFFFF) == 0 && fe.msg_index > 0)
                {
                    timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
                    const uint64_t now = static_cast<uint64_t>(t.tv_sec) * NS_PER_SEC + t.tv_nsec;
                    if (now - spin_start > NS_PER_SEC) { running = false; break; }
                }
                continue;
            }
            perror("recvmmsg"); running = false; break;
        }
        if (!running || n <= 0) break;

        for (int i = 0; i < n; ++i) { ptrs[i] = bufs[i]; lens[i] = msgs[i].msg_len; }
#ifdef QTIME
        for (int i = 0; i < n; ++i) rx_ns[i] = frontend_rx_ns(&msgs[i].msg_hdr);
        running = fe.process_batch(ptrs, lens, n, recv_cyc, spins, 0, rx_ns);
#else
        running = fe.process_batch(ptrs, lens, n, recv_cyc, spins, 0);
#endif
    }

    fe.finalize("../results/rung4a_latency.csv", "rung4a-busypoll", "spins", false);
    return 0;
}
