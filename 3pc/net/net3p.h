#pragma once

#include <emp-tool/emp-tool.h>
#include "3pc/core/types.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>

namespace threepc {

// ── Net3P ──────────────────────────────────────────────────────────────────────
/**
 * Network layer for 3-party communication.
 *
 * DESIGN: one DIRECTED (send-only) socket per ordered party pair → 6 sockets
 * total, 4 per party (2 outgoing, 2 incoming).
 *
 *   Directed edge   sender   receiver(server)   port
 *   ─────────────────────────────────────────────────
 *   P0 → P1         P0       P1                 base+0
 *   P0 → P2         P0       P2                 base+1
 *   P1 → P0         P1       P0                 base+2
 *   P1 → P2         P1       P2                 base+3
 *   P2 → P0         P2       P0                 base+4
 *   P2 → P1         P2       P1                 base+5
 *
 * Because sends and receives are on SEPARATE TCP sockets, any combination of
 * concurrent sends and receives across all three parties is deadlock-free by
 * construction — there is never a circular wait on a shared socket.
 *
 * Usage (three separate processes):
 *
 *   // Party 0
 *   Net3P net(P0, "127.0.0.1", "127.0.0.1", "127.0.0.1", 12340);
 *
 *   // Party 1
 *   Net3P net(P1, "127.0.0.1", "127.0.0.1", "127.0.0.1", 12340);
 *
 *   // Party 2
 *   Net3P net(P2, "127.0.0.1", "127.0.0.1", "127.0.0.1", 12340);
 *
 * All three can be started in any order; emp::NetIO retries client connections
 * until the server is ready.  Server sockets are started in background threads
 * so that no party blocks waiting for a client before it has connected its own
 * outgoing sockets.
 */
class Net3P {
 public:
  // ── Construction ─────────────────────────────────────────────────────────────
  /**
   * @param pid        This party's index (P0/P1/P2).
   * @param ips        IP address strings for P0, P1, P2 (length 3).
   * @param base_port  First of 6 consecutive ports.  Party p_i listens on two
   *                   of them and connects to two others (see table above).
   */
  Net3P(int pid, const char* ips[3], int base_port) : pid_(pid) {
    const int nxt = next_party(pid);
    const int prv = prev_party(pid);

    // Ports for the two outgoing directed edges from this party.
    const int port_to_nxt = directed_port(pid, nxt, base_port);
    const int port_to_prv = directed_port(pid, prv, base_port);

    // Ports for the two incoming directed edges to this party.
    const int port_from_nxt = directed_port(nxt, pid, base_port);
    const int port_from_prv = directed_port(prv, pid, base_port);

    // Start BOTH server (receive) sockets in background threads so this party
    // does not block before it has established its own outgoing connections.
    emp::NetIO* rx_nxt_ptr = nullptr;
    emp::NetIO* rx_prv_ptr = nullptr;

    std::thread t_rx_nxt([&]() {
      rx_nxt_ptr = new emp::NetIO(nullptr, port_from_nxt, /*quiet=*/true);
      rx_nxt_ptr->set_nodelay();
    });
    std::thread t_rx_prv([&]() {
      rx_prv_ptr = new emp::NetIO(nullptr, port_from_prv, /*quiet=*/true);
      rx_prv_ptr->set_nodelay();
    });

    // Connect outgoing sockets (emp::NetIO retries until the remote server
    // is ready, so order here does not matter).
    send_to_nxt_ = new emp::NetIO(ips[nxt], port_to_nxt, /*quiet=*/true);
    send_to_nxt_->set_nodelay();
    send_to_prv_ = new emp::NetIO(ips[prv], port_to_prv, /*quiet=*/true);
    send_to_prv_->set_nodelay();

    t_rx_nxt.join();
    t_rx_prv.join();
    recv_from_nxt_ = rx_nxt_ptr;
    recv_from_prv_ = rx_prv_ptr;
  }

  ~Net3P() {
    delete send_to_nxt_;
    delete send_to_prv_;
    delete recv_from_nxt_;
    delete recv_from_prv_;
  }

  Net3P(const Net3P&)            = delete;
  Net3P& operator=(const Net3P&) = delete;

  // ── Ring element send / recv ─────────────────────────────────────────────────

  /// Send `count` ring elements of type T to party `to`.
  template <typename T>
  void send_ring(const T* data, size_t count, int to) {
    bytes_sent_[to] += count * sizeof(T);
    send_io(to)->send_data(data, count * sizeof(T));
  }

  /// Receive `count` ring elements of type T from party `from` into `data`.
  template <typename T>
  void recv_ring(T* data, size_t count, int from) {
    bytes_recv_[from] += count * sizeof(T);
    recv_io(from)->recv_data(data, count * sizeof(T));
  }

  /// Send a single ring element to party `to`.
  template <typename T>
  void send_ring(T val, int to) {
    send_ring(&val, 1, to);
  }

  /// Receive and return a single ring element from party `from`.
  template <typename T>
  T recv_ring(int from) {
    T val{};
    recv_ring(&val, 1, from);
    return val;
  }

  // ── Raw byte send / recv ─────────────────────────────────────────────────────
  void send_bytes(const void* data, size_t len, int to) {
    bytes_sent_[to] += len;
    send_io(to)->send_data(data, len);
  }
  void recv_bytes(void* data, size_t len, int from) {
    bytes_recv_[from] += len;
    recv_io(from)->recv_data(data, len);
  }

  // ── Flush ────────────────────────────────────────────────────────────────────
  /// Flush all outgoing send buffers.
  void flush() {
    send_to_nxt_->flush();
    send_to_prv_->flush();
  }

  /// Flush the outgoing buffer toward party `to` only.
  void flush(int to) { send_io(to)->flush(); }

  // ── Stats / byte counters ─────────────────────────────────────────────────────
  /// Always 3 for Net3P. Exposed so generic CommPoint<Net> templates work
  /// uniformly and a future NetNP can use the same interface.
  int numParties() const { return 3; }

  int pid() const { return pid_; }

  /// Bytes sent to `party` since construction (or last resetCounters()).
  uint64_t bytesSentTo(int party) const {
    return bytes_sent_[static_cast<size_t>(party)];
  }

  /// Bytes received from `party` since construction (or last resetCounters()).
  uint64_t bytesRecvFrom(int party) const {
    return bytes_recv_[static_cast<size_t>(party)];
  }

  /// Reset per-party byte counters to zero (call between benchmark repetitions
  /// if you want per-run stats instead of cumulative totals).
  void resetCounters() {
    std::fill(std::begin(bytes_sent_), std::end(bytes_sent_), uint64_t{0});
    std::fill(std::begin(bytes_recv_), std::end(bytes_recv_), uint64_t{0});
  }

  // ── Socket buffer tuning ──────────────────────────────────────────────────────
  /**
   * Set SO_SNDBUF and SO_RCVBUF on every socket owned by this party to
   * `buffer_size` bytes.  The OS may cap the value at net.core.{r,w}mem_max;
   * the actual sizes are printed for the first socket so the caller can
   * detect kernel capping.
   *
   * Call immediately after construction, before any data is exchanged.
   *
   * Future NetNP: implement the same method signature — bench::increaseSocketBuffers()
   * in utils.h will pick it up automatically via the template.
   */
  void increaseSocketBuffers(int buffer_size) {
    // All four sockets this party owns (two outgoing, two incoming).
    emp::NetIO* sockets[4] = {
        send_to_nxt_, send_to_prv_,
        recv_from_nxt_, recv_from_prv_,
    };

    bool first = true;
    for (emp::NetIO* io : sockets) {
      if (!io) continue;
      int fd = io->sock;
      if (fd < 0) continue;

      int r1 = ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
      int r2 = ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));

      if (first) {
        first = false;
        if (r1 != 0 || r2 != 0)
          std::fprintf(stderr,
              "[Net3P P%d] Warning: setsockopt failed (errno %d: %s)\n",
              pid_, errno, std::strerror(errno));

        // Read back the actual kernel-allocated sizes (Linux doubles the value).
        int actual_snd = 0, actual_rcv = 0;
        socklen_t optlen = sizeof(int);
        ::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual_snd, &optlen);
        ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual_rcv, &optlen);

        std::printf("[Net3P P%d] Socket buffers — requested: %d B | "
                    "actual: SNDBUF=%d B  RCVBUF=%d B\n",
                    pid_, buffer_size, actual_snd, actual_rcv);

        if (actual_snd < buffer_size || actual_rcv < buffer_size) {
          std::fprintf(stderr,
              "[Net3P P%d] Warning: buffers capped by system limits. "
              "Consider: --sysctl net.core.rmem_max=%d --sysctl net.core.wmem_max=%d\n",
              pid_, buffer_size, buffer_size);
        }
      }
    }
  }

 private:
  // ── Port assignment ───────────────────────────────────────────────────────────
  /**
   * Canonical port for the directed edge sender→receiver.
   * Encoding (6 directed edges, densely packed):
   *   (0→1): base+0,  (0→2): base+1
   *   (1→0): base+2,  (1→2): base+3
   *   (2→0): base+4,  (2→1): base+5
   *
   * Formula: base + sender*2 + (receiver > sender ? receiver-1 : receiver)
   */
  static int directed_port(int sender, int receiver, int base) {
    return base + sender * 2 + (receiver > sender ? receiver - 1 : receiver);
  }

  // ── Socket routing ────────────────────────────────────────────────────────────
  emp::NetIO* send_io(int to) {
    if (to == next_party(pid_)) return send_to_nxt_;
    if (to == prev_party(pid_)) return send_to_prv_;
    throw std::invalid_argument("Net3P::send_io: invalid destination party");
  }

  emp::NetIO* recv_io(int from) {
    if (from == next_party(pid_)) return recv_from_nxt_;
    if (from == prev_party(pid_)) return recv_from_prv_;
    throw std::invalid_argument("Net3P::recv_io: invalid source party");
  }

  int pid_;

  // ── Per-party byte counters ───────────────────────────────────────────────────
  // Indexed by party id (0..2).  Self-slot is never incremented.
  uint64_t bytes_sent_[3]{};
  uint64_t bytes_recv_[3]{};

  // Outgoing sockets (this party is client / sender).
  emp::NetIO* send_to_nxt_{nullptr};   // directed edge: pid → next_party(pid)
  emp::NetIO* send_to_prv_{nullptr};   // directed edge: pid → prev_party(pid)

  // Incoming sockets (this party is server / receiver).
  emp::NetIO* recv_from_nxt_{nullptr}; // directed edge: next_party(pid) → pid
  emp::NetIO* recv_from_prv_{nullptr}; // directed edge: prev_party(pid) → pid
};

}  // namespace threepc
