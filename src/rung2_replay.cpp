#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "frontend.h"

constexpr int BATCH = 64;   // packets drained per recvmmsg syscall

int main(int argc, char** argv)
{
    bool emit = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--emit") emit = true;

    mlockall(MCL_CURRENT | MCL_FUTURE);
    if (!pin_to_core(6)) std::cerr << "warning: could not pin to core 6\n";

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    const int rcvbuf = 128 * 1024 * 1024;  // kernel clamps to net.core.rmem_max
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    const timeval rcvto{1, 0};             // 1 s idle → self-terminate (dropped-EOS backstop)
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));
#ifdef QTIME
    const int on = 1;                      // kernel RX timestamps for queueing latency
    setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MOLD_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { perror("bind"); return 1; }

    // Preallocated + pre-faulted recvmmsg machinery: one iovec/buffer per slot.
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

    Frontend fe;   // constructed after pin — calibrates the timer
    if (emit) fe.open_tob("../results/rung2_AAPL.tob");

    const uint8_t* ptrs[BATCH];
    unsigned lens[BATCH];
    bool running = true;
    while (running)
    {
        // Blocking: MSG_WAITFORONE returns once >=1 packet is ready, draining
        // whatever else is queued (up to BATCH). Not MSG_DONTWAIT — busy-poll rung.
#ifdef QTIME
        for (int i = 0; i < BATCH; ++i) msgs[i].msg_hdr.msg_controllen = sizeof(ctrl[i]);
#endif
#ifdef MEASURE
        const uint64_t r0 = now_cycles();
#endif
        const int n = recvmmsg(fd, msgs, BATCH, MSG_WAITFORONE, nullptr);
#ifdef MEASURE
        const uint32_t recv_cyc = static_cast<uint32_t>(now_cycles() - r0);
#else
        const uint32_t recv_cyc = 0;
#endif
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            { if (fe.msg_index > 0) break; else continue; }   // idle: done if started, else wait
            perror("recvmmsg"); break;
        }

        for (int i = 0; i < n; ++i) { ptrs[i] = bufs[i]; lens[i] = msgs[i].msg_len; }
#ifdef QTIME
        for (int i = 0; i < n; ++i) rx_ns[i] = frontend_rx_ns(&msgs[i].msg_hdr);
        running = fe.process_batch(ptrs, lens, n, recv_cyc, 0, 0, rx_ns);
#else
        running = fe.process_batch(ptrs, lens, n, recv_cyc, 0, 0);
#endif
    }

    fe.finalize("../results/rung2_latency.csv", "rung2-blocking");
    return 0;
}
