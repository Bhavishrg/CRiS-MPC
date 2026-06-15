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

  void increaseSocketBuffers(int buffer_size) {
    bool first = true;

    auto tune = [&](emp::NetIO* io) {
      if (!io || io->sock < 0) return;
      int fd = io->sock;
      int r1 = ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                            &buffer_size, sizeof(buffer_size));
      int r2 = ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                            &buffer_size, sizeof(buffer_size));

      if (first) {
        first = false;
        if (r1 != 0 || r2 != 0) {
          std::fprintf(stderr,
              "[NetNP P%d] Warning: setsockopt failed (errno %d: %s)\n",
              pid_, errno, std::strerror(errno));
        }

        int actual_snd = 0, actual_rcv = 0;
        socklen_t optlen = sizeof(int);
        ::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual_snd, &optlen);
        ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual_rcv, &optlen);

        std::printf("[NetNP P%d] Socket buffers — requested: %d B | "
                    "actual: SNDBUF=%d B  RCVBUF=%d B\n",
                    pid_, buffer_size, actual_snd, actual_rcv);

        if (actual_snd < buffer_size || actual_rcv < buffer_size) {
          std::fprintf(stderr,
              "[NetNP P%d] Warning: buffers capped by system limits. "
              "Consider: --sysctl net.core.rmem_max=%d --sysctl net.core.wmem_max=%d\n",
              pid_, buffer_size, buffer_size);
        }
      }
    };

    for (auto* io : send_ios_) tune(io);
    for (auto* io : recv_ios_) tune(io);
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
