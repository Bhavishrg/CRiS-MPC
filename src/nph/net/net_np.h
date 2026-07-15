#pragma once

#include <emp-tool/emp-tool.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>

namespace threepc::nph {

/**
 * NetNP — directed-socket network for an arbitrary number of processes.
 *
 * This is the n-party analogue of Net3P.  For every ordered pair i -> j,
 * i != j, there is one directed TCP socket.  Therefore all parties may send
 * first, flush, and then receive without deadlock.
 *
 * In the n-party-with-helper protocol, processes are numbered as follows:
 *   compute parties: 0, 1, ..., n-1
 *   helper:          n
 *   total processes: n+1
 */
class NetNP {
 public:
  NetNP(int pid, int total_parties, const std::string& peer, int base_port)
      : pid_(pid), total_parties_(total_parties),
        send_ios_(static_cast<size_t>(total_parties), nullptr),
        recv_ios_(static_cast<size_t>(total_parties), nullptr),
        bytes_sent_(static_cast<size_t>(total_parties), 0),
        bytes_recv_(static_cast<size_t>(total_parties), 0) {
    if (total_parties_ <= 1)
      throw std::invalid_argument("NetNP: total_parties must be at least 2");
    if (pid_ < 0 || pid_ >= total_parties_)
      throw std::invalid_argument("NetNP: invalid pid");

    std::vector<std::string> ips(static_cast<size_t>(total_parties_), peer);

    // Start all incoming server sockets first, one per source party.
    std::vector<std::thread> server_threads;
    server_threads.reserve(static_cast<size_t>(total_parties_ - 1));

    for (int from = 0; from < total_parties_; ++from) {
      if (from == pid_) continue;
      const int port = directed_port(from, pid_, base_port);
      server_threads.emplace_back([this, from, port]() {
        emp::NetIO* io = new emp::NetIO(nullptr, port, /*quiet=*/true);
        io->set_nodelay();
        recv_ios_[static_cast<size_t>(from)] = io;
      });
    }

    // Connect all outgoing client sockets.
    for (int to = 0; to < total_parties_; ++to) {
      if (to == pid_) continue;
      const int port = directed_port(pid_, to, base_port);
      emp::NetIO* io = new emp::NetIO(ips[static_cast<size_t>(to)].c_str(),
                                      port,
                                      /*quiet=*/true);
      io->set_nodelay();
      send_ios_[static_cast<size_t>(to)] = io;
    }

    for (auto& t : server_threads) t.join();
  }

  ~NetNP() {
    for (auto* io : send_ios_) delete io;
    for (auto* io : recv_ios_) delete io;
  }

  NetNP(const NetNP&) = delete;
  NetNP& operator=(const NetNP&) = delete;

  int pid() const { return pid_; }
  int numParties() const { return total_parties_; }

  template <typename T>
  void send_ring(const T* data, size_t count, int to) {
    check_peer(to);
    bytes_sent_[static_cast<size_t>(to)] += count * sizeof(T);
    send_ios_[static_cast<size_t>(to)]->send_data(data, count * sizeof(T));
  }

  template <typename T>
  void recv_ring(T* data, size_t count, int from) {
    check_peer(from);
    bytes_recv_[static_cast<size_t>(from)] += count * sizeof(T);
    recv_ios_[static_cast<size_t>(from)]->recv_data(data, count * sizeof(T));
  }

  template <typename T>
  void send_ring(T val, int to) {
    send_ring(&val, 1, to);
  }

  template <typename T>
  T recv_ring(int from) {
    T val{};
    recv_ring(&val, 1, from);
    return val;
  }

  void send_bytes(const void* data, size_t len, int to) {
    check_peer(to);
    bytes_sent_[static_cast<size_t>(to)] += len;
    send_ios_[static_cast<size_t>(to)]->send_data(data, len);
  }

  void recv_bytes(void* data, size_t len, int from) {
    check_peer(from);
    bytes_recv_[static_cast<size_t>(from)] += len;
    recv_ios_[static_cast<size_t>(from)]->recv_data(data, len);
  }

  void flush() {
    for (int to = 0; to < total_parties_; ++to) {
      if (to == pid_) continue;
      send_ios_[static_cast<size_t>(to)]->flush();
    }
  }

  void flush(int to) {
    check_peer(to);
    send_ios_[static_cast<size_t>(to)]->flush();
  }

  uint64_t bytesSentTo(int party) const {
    return bytes_sent_.at(static_cast<size_t>(party));
  }

  uint64_t bytesRecvFrom(int party) const {
    return bytes_recv_.at(static_cast<size_t>(party));
  }

  void resetCounters() {
    std::fill(bytes_sent_.begin(), bytes_sent_.end(), uint64_t{0});
    std::fill(bytes_recv_.begin(), bytes_recv_.end(), uint64_t{0});
  }

  /**
   * Send/receive `bytes_per_peer` bytes of dummy data on every directed
   * socket to this party, restricted to peers with id < peer_limit
   * (exclusive). Pass the number of compute parties here to warm up only
   * the compute-party mesh/star connections without requiring the helper
   * (which does not call this) to participate.
   *
   * Linux resets the TCP congestion window after a socket goes idle
   * (net.ipv4.tcp_slow_start_after_idle, on by default). If a socket's first
   * real bulk transfer happens after an idle gap (e.g. after a CPU-bound
   * preprocessing phase), that transfer pays the full slow-start ramp-up
   * cost under high RTT before reaching its steady-state throughput. Calling
   * this immediately before a latency-sensitive bulk phase keeps the
   * connection "hot" so the real transfer does not have to re-ramp from a
   * cold congestion window. Byte counters are not affected — call
   * resetCounters() after this if a clean slate is desired.
   */
  void warmup(size_t bytes_per_peer, int peer_limit) {
    if (bytes_per_peer == 0) return;
    if (peer_limit < 0 || peer_limit > total_parties_)
      throw std::invalid_argument("NetNP::warmup: invalid peer_limit");
    if (pid_ >= peer_limit) return;

    std::vector<uint8_t> send_buf(bytes_per_peer, 0);
    for (int to = 0; to < peer_limit; ++to) {
      if (to == pid_) continue;
      send_bytes(send_buf.data(), send_buf.size(), to);
    }
    flush();

    std::vector<uint8_t> recv_buf(bytes_per_peer);
    for (int from = 0; from < peer_limit; ++from) {
      if (from == pid_) continue;
      recv_bytes(recv_buf.data(), recv_buf.size(), from);
    }
  }

  /**
   * Set SO_SNDBUF and SO_RCVBUF on every socket owned by this party to
   * `buffer_size` bytes. emp::NetIO exposes the underlying fd via its
   * public `consocket` member, so this sets the option directly.
   *
   * The kernel may cap the value at net.core.{r,w}mem_max (and doubles
   * whatever it grants for bookkeeping overhead); if large sends/recvs
   * still stall under high-latency/bandwidth-limited networks, raise
   * those sysctls too, e.g.:
   *   sudo sysctl -w net.core.rmem_max=<bytes> net.core.wmem_max=<bytes>
   *
   * Call immediately after construction, before any data is exchanged.
   */
  void increaseSocketBuffers(int buffer_size) {
    auto set_buf = [&](emp::NetIO* io, int party) {
      if (io == nullptr || io->consocket < 0) return;
      const int fd = io->consocket;
      if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) != 0) {
        std::fprintf(stderr,
            "[NetNP P%d] setsockopt(SO_SNDBUF) to party %d failed: %s\n",
            pid_, party, std::strerror(errno));
      }
      if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) != 0) {
        std::fprintf(stderr,
            "[NetNP P%d] setsockopt(SO_RCVBUF) to party %d failed: %s\n",
            pid_, party, std::strerror(errno));
      }
    };

    for (int to = 0; to < total_parties_; ++to) {
      if (to == pid_) continue;
      set_buf(send_ios_[static_cast<size_t>(to)], to);
    }
    for (int from = 0; from < total_parties_; ++from) {
      if (from == pid_) continue;
      set_buf(recv_ios_[static_cast<size_t>(from)], from);
    }

    // Report the actual granted size for the first socket so callers can
    // detect kernel capping via net.core.{r,w}mem_max.
    for (int to = 0; to < total_parties_; ++to) {
      if (to == pid_) continue;
      emp::NetIO* io = send_ios_[static_cast<size_t>(to)];
      if (io == nullptr || io->consocket < 0) continue;
      int actual_sndbuf = 0;
      socklen_t len = sizeof(actual_sndbuf);
      getsockopt(io->consocket, SOL_SOCKET, SO_SNDBUF, &actual_sndbuf, &len);
      std::fprintf(stderr,
          "[NetNP P%d] Requested SO_SNDBUF=%d, kernel granted=%d "
          "(if smaller than requested, raise net.core.wmem_max/rmem_max).\n",
          pid_, buffer_size, actual_sndbuf);
      break;
    }
  }

 private:
  static int directed_port(int sender, int receiver, int base) {
    // Dense encoding of all directed edges for a complete directed graph.
    // For fixed sender, receivers are packed in increasing order excluding self.
    const int recv_index = receiver > sender ? receiver - 1 : receiver;
    return base + sender * 1024 + recv_index;
  }

  void check_peer(int party) const {
    if (party < 0 || party >= total_parties_ || party == pid_)
      throw std::invalid_argument("NetNP: invalid peer party");
  }

  int pid_;
  int total_parties_;
  std::vector<emp::NetIO*> send_ios_;
  std::vector<emp::NetIO*> recv_ios_;
  std::vector<uint64_t> bytes_sent_;
  std::vector<uint64_t> bytes_recv_;
};

}  // namespace threepc::nph
