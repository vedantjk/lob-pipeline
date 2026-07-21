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
#include <liburing.h>

#include "frontend.h"

constexpr int NBUF = 4096;   // provided buffers (power of 2)
constexpr int BGID = 1;      // buffer group id
constexpr int QD = 8192;     // io_uring queue depth (>= NBUF for pending CQEs)

// Provided-buffer pool. Multishot recv delivers one datagram per buffer.
static uint8_t g_buffers[NBUF][MOLD_MAX_PACKET];

int main(int argc, char** argv)
{
    bool emit = false, sqpoll = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--emit") emit = true;
        else if (a == "--sqpoll") sqpoll = true;
    }

    mlockall(MCL_CURRENT | MCL_FUTURE);
    if (!pin_to_core(6)) std::cerr << "warning: could not pin to core 6\n";

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    const int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const int rcvbuf = 128 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MOLD_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { perror("bind"); return 1; }

    io_uring ring;
    io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SINGLE_ISSUER;
    if (sqpoll)
    {
        // SQPOLL: a kernel thread polls the SQ — no submit syscalls. It costs a
        // core; pin it off the reader(6)/sender(7) cores. DEFER_TASKRUN is
        // incompatible with SQPOLL, so it's only used in the default path.
        params.flags |= IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
        params.sq_thread_cpu = 5;
        params.sq_thread_idle = 1000;
    }
    else
    {
        params.flags |= IORING_SETUP_DEFER_TASKRUN;
    }
    if (io_uring_queue_init_params(QD, &ring, &params) < 0) { perror("queue_init"); return 1; }

    if (io_uring_register_files(&ring, &fd, 1) < 0) { perror("register_files"); return 1; }

    int ret = 0;
    io_uring_buf_ring* br = io_uring_setup_buf_ring(&ring, NBUF, BGID, 0, &ret);
    if (!br) { errno = -ret; perror("setup_buf_ring"); return 1; }
    std::memset(g_buffers, 0, sizeof(g_buffers));
    const int mask = io_uring_buf_ring_mask(NBUF);
    for (int i = 0; i < NBUF; ++i)
        io_uring_buf_ring_add(br, g_buffers[i], MOLD_MAX_PACKET, i, mask, i);
    io_uring_buf_ring_advance(br, NBUF);

    // Arm a multishot recv: prep only — the loop's submit_and_wait submits it.
    auto arm = [&] {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        io_uring_prep_recv_multishot(sqe, 0, nullptr, 0, 0);   // fd index 0 (fixed)
        sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
        sqe->buf_group = BGID;
    };
    arm();

    Frontend fe;
    if (emit) fe.open_tob("../results/rung4b_AAPL.tob");

    const uint8_t* ptrs[NBUF];
    unsigned lens[NBUF];
    int bids[NBUF];
    bool running = true;
    while (running)
    {
        // recv_cyc = submit→completion: submits the (re-)armed multishot and
        // waits for >=1 CQE. Under SQPOLL this often does no syscall at all.
#ifdef MEASURE
        const uint64_t r0 = now_cycles();
#endif
        io_uring_cqe* wcqe = nullptr;
        __kernel_timespec ts{1, 0};   // 1 s idle backstop
        const int wret = io_uring_submit_and_wait_timeout(&ring, &wcqe, 1, &ts, nullptr);
#ifdef MEASURE
        const uint32_t recv_cyc = static_cast<uint32_t>(now_cycles() - r0);
#else
        const uint32_t recv_cyc = 0;
#endif
        if (wret < 0 && wret == -ETIME) { if (fe.msg_index > 0) break; else continue; }
        unsigned head, nseen = 0;
        int cnt = 0;
        bool need_rearm = false;
        io_uring_cqe* cqe;
        io_uring_for_each_cqe(&ring, head, cqe)
        {
            ++nseen;
            if (!(cqe->flags & IORING_CQE_F_MORE)) need_rearm = true;   // multishot ended
            if (cqe->res > 0 && (cqe->flags & IORING_CQE_F_BUFFER))
            {
                const int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                ptrs[cnt] = g_buffers[bid];
                lens[cnt] = static_cast<unsigned>(cqe->res);
                bids[cnt] = bid;
                ++cnt;
            }
            // res<=0 (e.g. -ENOBUFS) carries no buffer — nothing to recycle.
        }

        if (cnt > 0)
            running = fe.process_batch(ptrs, lens, cnt, recv_cyc, sqpoll ? 1 : 0, 0);

        // Return consumed buffers to the ring only after process_batch read them.
        for (int i = 0; i < cnt; ++i)
            io_uring_buf_ring_add(br, g_buffers[bids[i]], MOLD_MAX_PACKET, bids[i], mask, i);
        if (cnt > 0) io_uring_buf_ring_advance(br, cnt);
        io_uring_cq_advance(&ring, nseen);

        if (need_rearm && running) arm();
    }

    fe.finalize("../results/rung4b_latency.csv", sqpoll ? "rung4b-iouring-sqpoll" : "rung4b-iouring",
                "sqpoll", false);
    io_uring_queue_exit(&ring);
    return 0;
}
