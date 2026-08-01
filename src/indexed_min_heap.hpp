#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace standalone_decimator {

template<typename Value> class IndexedMinHeap {
 public:
  struct Entry {
    float key = 0.0f;
    Value value{};
  };

  bool empty() const noexcept
  {
    return heap_.empty();
  }

  std::size_t size() const noexcept
  {
    return heap_.size();
  }

  void clear() noexcept
  {
    for (const Entry &entry : heap_) {
      positions_[value_index(entry.value)] = kMissing;
    }
    heap_.clear();
  }

  void reserve(const std::size_t capacity)
  {
    if (capacity > kMaxHeapSize) {
      throw std::length_error("indexed heap capacity exceeds 32-bit positions");
    }
    heap_.reserve(capacity);
  }

  void reserve_values(const std::size_t value_count)
  {
    if (value_count > positions_.size()) {
      positions_.resize(value_count, kMissing);
    }
  }

  void prepare(const std::size_t value_count)
  {
    reserve(value_count);
    reserve_values(value_count);
  }

  void update(const Value value, const float key)
  {
    const std::size_t index = value_index(value);
    if (index >= positions_.size() || positions_[index] == kMissing) {
      insert_at_index(value, key, index);
      return;
    }

    const std::size_t position = positions_[index];
    const float previous_key = heap_[position].key;
    if (key < previous_key) {
      heap_[position].key = key;
      sift_up(position);
    }
    else if (key > previous_key) {
      heap_[position].key = key;
      sift_down(position);
    }
  }

  bool remove(const Value value)
  {
    const std::size_t index = value_index(value);
    if (index >= positions_.size() || positions_[index] == kMissing) {
      return false;
    }

    std::size_t position = positions_[index];
    if (position > 0) {
      const Entry entry = heap_[position];
      do {
        const std::size_t parent_position = parent(position);
        heap_[position] = heap_[parent_position];
        positions_[value_index(heap_[position].value)] = static_cast<Position>(position);
        position = parent_position;
      } while (position > 0);
      heap_.front() = entry;
      positions_[index] = 0;
    }

    pop_min();
    return true;
  }

  Entry pop()
  {
    if (heap_.empty()) {
      throw std::runtime_error("pop() called on an empty indexed heap");
    }
    return pop_min();
  }

 private:
  using Position = std::uint32_t;
  static constexpr Position kMissing = std::numeric_limits<Position>::max();
  static constexpr std::size_t kMaxHeapSize = static_cast<std::size_t>(kMissing);

  static std::size_t value_index(const Value value) noexcept
  {
    return static_cast<std::size_t>(value);
  }

  static std::size_t parent(const std::size_t position) noexcept
  {
    return (position - 1) / 2;
  }

  static std::size_t left_child(const std::size_t position) noexcept
  {
    return position * 2 + 1;
  }

  static bool cost_less(const Entry &left, const Entry &right) noexcept
  {
    return left.key < right.key;
  }

  void ensure_position(const std::size_t index)
  {
    if (index >= positions_.size()) {
      if (index == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("indexed heap value index exceeds addressable range");
      }
      reserve_values(index + 1);
    }
  }

  void insert_at_index(const Value value, const float key, const std::size_t index)
  {
    ensure_position(index);
    if (positions_[index] != kMissing) {
      throw std::runtime_error("duplicate indexed heap insertion");
    }
    if (heap_.size() >= kMaxHeapSize) {
      throw std::length_error("indexed heap exceeds 32-bit positions");
    }

    const std::size_t position = heap_.size();
    heap_.push_back({key, value});
    positions_[index] = static_cast<Position>(position);
    sift_up(position);
  }

  Entry pop_min()
  {
    const Entry result = heap_.front();
    if (heap_.size() > 1) {
      heap_.front() = heap_.back();
      positions_[value_index(heap_.front().value)] = 0;
    }
    heap_.pop_back();
    positions_[value_index(result.value)] = kMissing;
    if (!heap_.empty()) {
      sift_down(0);
    }
    return result;
  }

  void sift_up(std::size_t position) noexcept
  {
    if (position == 0) {
      return;
    }

    std::size_t parent_position = parent(position);
    if (cost_less(heap_[parent_position], heap_[position])) {
      return;
    }

    const Entry entry = heap_[position];
    do {
      heap_[position] = heap_[parent_position];
      positions_[value_index(heap_[position].value)] = static_cast<Position>(position);
      position = parent_position;
      if (position == 0) {
        break;
      }
      parent_position = parent(position);
    } while (!cost_less(heap_[parent_position], entry));

    heap_[position] = entry;
    positions_[value_index(entry.value)] = static_cast<Position>(position);
  }

  std::size_t sift_down_child(const std::size_t position,
                              const Entry &entry,
                              const std::size_t heap_size) const noexcept
  {
    const std::size_t left = left_child(position);
    if (left >= heap_size) {
      return position;
    }

    const std::size_t right = left + 1;
    std::size_t smallest = position;
    if (cost_less(heap_[left], entry)) {
      smallest = left;
    }
    if (right < heap_size &&
        cost_less(heap_[right], smallest == position ? entry : heap_[smallest])) {
      smallest = right;
    }
    return smallest;
  }

  void sift_down(std::size_t position) noexcept
  {
    const std::size_t heap_size = heap_.size();
    std::size_t smallest = sift_down_child(position, heap_[position], heap_size);
    if (smallest == position) {
      return;
    }

    const Entry entry = heap_[position];
    do {
      heap_[position] = heap_[smallest];
      positions_[value_index(heap_[position].value)] = static_cast<Position>(position);
      position = smallest;
      smallest = sift_down_child(position, entry, heap_size);
    } while (smallest != position);

    heap_[position] = entry;
    positions_[value_index(entry.value)] = static_cast<Position>(position);
  }

  std::vector<Entry> heap_;
  std::vector<Position> positions_;
};

}  // namespace standalone_decimator
