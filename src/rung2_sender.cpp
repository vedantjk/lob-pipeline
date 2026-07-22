#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mold.h"
#include "parser.h"   // MAX_PAYLOAD (block-length sanity bound)

constexpr int SBATCH = 64;   // packets per sendmmsg syscall

static uint64_t now_ns()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * NS_PER_SEC + ts.tv_nsec;
}

// Per-send-batch machinery (sendmmsg drains many packets per syscall — the send
// side has to outrun the receiver to stress it, so one-sendto-per-packet is out).
static uint8_t pktbuf[SBATCH][MOLD_MAX_PACKET];
static mmsghdr smsgs[SBATCH];
static iovec siovs[SBATCH];

int main(int argc, char** argv)
{
    const char* input = nullptr;
    uint64_t rate = 0;       // target messages/sec; 0 = wide open
    uint64_t duration = 0;   // seconds to send before stopping; 0 = whole file
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = std::stoull(argv[++i]);
        else if (a == "--duration" && i + 1 < argc) duration = std::stoull(argv[++i]);
        else input = argv[i];
    }
    if (!input) { std::cerr << "Usage: " << argv[0] << " <itch_file> [--rate N] [--duration secs]\n"; return 1; }

    std::ifstream file(input, std::ios::binary);
    if (!file) { std::cerr << "Failed to open " << input << "\n"; return 1; }

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    const int sndbuf = 32 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(MOLD_PORT);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < SBATCH; ++i)
    {
        std::memset(&smsgs[i], 0, sizeof(smsgs[i]));
        siovs[i].iov_base = pktbuf[i];
        smsgs[i].msg_hdr.msg_iov = &siovs[i];
        smsgs[i].msg_hdr.msg_iovlen = 1;
        smsgs[i].msg_hdr.msg_name = &dst;
        smsgs[i].msg_hdr.msg_namelen = sizeof(dst);
    }
    int pkt = 0;                          // current packet slot in the send-batch
    std::size_t off = MOLD_HEADER_SIZE;   // write cursor within pktbuf[pkt]
    uint16_t count = 0;                   // messages in the current packet
    uint64_t seq_first = MOLD_FIRST_SEQ;  // seq of the current packet's first msg
    uint64_t next_seq = MOLD_FIRST_SEQ;   // seq the next message will take
    uint64_t packets = 0;
    const uint64_t start_ns = now_ns();
    const uint64_t deadline = duration ? start_ns + duration * NS_PER_SEC : 0;

    auto send_batch = [&] {
        if (pkt == 0) return;
        int sent = 0;
        while (sent < pkt)   // sendmmsg may send a prefix; loop the remainder
        {
            const int r = sendmmsg(fd, smsgs + sent, pkt - sent, 0);
            if (r < 0) { perror("sendmmsg"); std::exit(1); }
            sent += r;
        }
        packets += pkt;
        pkt = 0;
        if (rate)   // pace per send-batch against wall-clock
        {
            const uint64_t target = start_ns + (next_seq - MOLD_FIRST_SEQ) * NS_PER_SEC / rate;
            while (now_ns() < target) { /* spin; sub-ms sleeps aren't reliable */ }
        }
    };

    auto finalize_packet = [&] {
        if (count == 0) return;
        mold_pack_header(pktbuf[pkt], seq_first, count);
        siovs[pkt].iov_len = off;
        ++pkt;
        seq_first = next_seq;
        off = MOLD_HEADER_SIZE;
        count = 0;
        if (pkt == SBATCH) send_batch();
    };

    uint8_t msg[2048], len_buf[2];
    while (file.read(reinterpret_cast<char*>(len_buf), 2))
    {
        const uint16_t n = static_cast<uint16_t>(read_be<2>(len_buf));
        // Sanity-bound n before copying: it's a 16-bit prefix from the file (up to
        // 65535). Unchecked, file.read overflows msg[] and the later memcpy
        // overflows pktbuf. A valid ITCH body is < MAX_PAYLOAD; anything larger is
        // a corrupt/desynced capture — stop rather than smash memory.
        if (n > MAX_PAYLOAD) { std::cerr << "oversize block len=" << n << ", stopping\n"; break; }
        if (!file.read(reinterpret_cast<char*>(msg), n)) break;

        const std::size_t block = 2 + n;
        if (off + block > MOLD_MAX_PACKET)
        {
            finalize_packet();
            if (deadline && now_ns() >= deadline) break;
        }
        write_be<2>(pktbuf[pkt] + off, n);
        std::memcpy(pktbuf[pkt] + off + 2, msg, n);
        off += block;
        ++count;
        ++next_seq;
    }
    finalize_packet();
    send_batch();

    // Redundant end-of-session (a single sentinel can drop under saturation; the
    // receiver also self-terminates on idle, so this is belt-and-suspenders).
    mold_pack_header(pktbuf[0], next_seq, 0xFFFF);
    for (int i = 0; i < 64; ++i)
        sendto(fd, pktbuf[0], MOLD_HEADER_SIZE, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));

    std::cerr << "sent " << (next_seq - MOLD_FIRST_SEQ) << " messages in "
              << packets << " packets\n";
    close(fd);
    return 0;
}
