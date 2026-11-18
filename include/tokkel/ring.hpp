/**
 ******************************************************************************
 * One ring buffer to rule the mall
 ******************************************************************************
 */

#pragma once

#include <atomic>
#include <cstddef>

namespace tokkel {
template <typename T, std::size_t N> class Ring {
private:
  T data[N];
  std::size_t head{0};
  std::size_t tail{0};
  std::atomic<std::size_t> len{0};

public:
  void push_to_ring(const T &b) {
    if (len != N) {
      data[head] = b;
      head = (head + 1) % N;
      len++;
    }
  }

  std::size_t peek_ring(T **out) {
    const std::size_t l = len.load(std::memory_order_acquire);
    if (l == 0)
      return 0;
    *out = &data[tail];
    std::size_t distance = N - tail;
    if (distance > l)
      return l;
    else
      return distance;
  }

  void advance_ring(std::size_t n) {
    tail = ((tail + n) % N);
    len -= n;
  }
};
} // namespace tokkel
