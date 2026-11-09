/** @file Observe physical PPS edges without resetting time during a stream. */
#pragma once
#include <uhd/usrp/multi_usrp.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "protocol.hpp"

namespace bistatic {
using Steady = std::chrono::steady_clock;
extern std::atomic<bool> interrupted;

struct Edge { uint64_t sequence; double time; };

/** Local edge numbers deliberately need not agree between the two stations. */
class PpsClock {
  uhd::usrp::multi_usrp::sptr radio;
  double max_ppm;
  std::thread worker;
  std::atomic<bool> stop{false};
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::deque<Edge> edges;
  std::deque<double> periods;
  std::string failure;

  void observe() {
    try {
      double previous = radio->get_time_last_pps().get_real_secs();
      auto last_seen = Steady::now();
      while (!stop && !interrupted) {
        double current = radio->get_time_last_pps().get_real_secs();
        if (current != previous) {
          std::lock_guard<std::mutex> lock(mutex);
          if (!edges.empty()) {
            double period = current - previous;
            if (!std::isfinite(period) || std::abs(period - 1) * 1e6 > max_ppm)
              throw std::runtime_error("PPS jump or clock-rate error exceeds configured limit");
            periods.push_back(period);
            if (periods.size() > 8) periods.pop_front();
          }
          edges.push_back({edges.empty() ? 1 : edges.back().sequence + 1, current});
          if (edges.size() > 128) edges.pop_front();
          previous = current;
          last_seen = Steady::now();
          changed.notify_all();
        }
        if (Steady::now() - last_seen > std::chrono::milliseconds(2500)) throw std::runtime_error("Missing external 1 Hz PPS");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex);
      failure = error.what();
      changed.notify_all();
    }
  }

 public:
  PpsClock(uhd::usrp::multi_usrp::sptr device, double limit) : radio(device), max_ppm(limit), worker() {
    worker = std::thread([this] { observe(); });
  }
  ~PpsClock() { stop = true; if (worker.joinable()) worker.join(); }

  void check() const {
    if (interrupted) throw std::runtime_error("Interrupted");
    std::lock_guard<std::mutex> lock(mutex);
    if (!failure.empty()) throw std::runtime_error(failure);
  }

  /** Estimate radio seconds per physical second from recent measured PPS intervals. */
  double ratio() const {
    check();
    std::lock_guard<std::mutex> lock(mutex);
    if (periods.size() < 2) throw std::runtime_error("PPS clock has not acquired three edges yet");
    double sum = 0;
    for (double period : periods) sum += period;
    return sum / periods.size();
  }

  Edge waitEdge(uint64_t sequence, double timeout = 5) {
    auto deadline = Steady::now() + std::chrono::duration<double>(timeout);
    std::unique_lock<std::mutex> lock(mutex);
    while (!interrupted && failure.empty() && (edges.empty() || edges.back().sequence < sequence)) {
      changed.wait_for(lock, std::chrono::milliseconds(20));
      if (Steady::now() >= deadline) throw std::runtime_error("PPS edge wait timed out");
    }
    if (interrupted) throw std::runtime_error("Interrupted");
    if (!failure.empty()) throw std::runtime_error(failure);
    for (const auto& edge : edges) if (edge.sequence == sequence) return edge;
    throw std::runtime_error("Requested PPS edge is outside the retained history");
  }

  Edge latest() const {
    check();
    std::lock_guard<std::mutex> lock(mutex);
    if (edges.empty()) throw std::runtime_error("No PPS edge acquired");
    return edges.back();
  }

  /** Associate an RF packet timestamp with the physical PPS edge before it. */
  Edge before(double timestamp) const {
    check();
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = edges.rbegin(); it != edges.rend(); ++it) if (it->time <= timestamp) return *it;
    throw std::runtime_error("RF timestamp predates the PPS history");
  }
};
}  // namespace bistatic
