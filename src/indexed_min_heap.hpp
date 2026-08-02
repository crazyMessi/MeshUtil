#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace standalone_decimator {

template<typename Value> class IndexedMinHeap {
 public:
  using Position = std::uint32_t;
  using PositionStorage = std::vector<Position>;
  static constexpr Position kMissingPosition =
      std::numeric_limits<Position>::max();

  struct Entry {
    float key = 0.0f;
    Value value{};
  };

  IndexedMinHeap() noexcept : positions_(owned_positions_) {}

  explicit IndexedMinHeap(PositionStorage &positions,
                          const Position heap_tag = 0,
                          const Position heap_tag_count = 1)
      : positions_(positions),
        heap_tag_(heap_tag),
        tagged_positions_(true)
  {
    if (heap_tag_count == 0 || heap_tag_ >= heap_tag_count) {
      throw std::invalid_argument("indexed heap tag is out of range");
    }
    Position value = heap_tag_count;
    do {
      ++tag_bits_;
      value >>= 1;
    } while (value != 0);
    tag_mask_ = (Position{1} << tag_bits_) - 1;
    if (heap_tag_ + 1 > tag_mask_) {
      throw std::invalid_argument("indexed heap tag exceeds encoded capacity");
    }
  }

  ~IndexedMinHeap()
  {
    if (&positions_ != &owned_positions_) {
      clear();
    }
  }

  IndexedMinHeap(const IndexedMinHeap &) = delete;
  IndexedMinHeap &operator=(const IndexedMinHeap &) = delete;
  IndexedMinHeap(IndexedMinHeap &&) = delete;
  IndexedMinHeap &operator=(IndexedMinHeap &&) = delete;

  PositionStorage &position_storage() noexcept
  {
    return positions_;
  }

  const PositionStorage &position_storage() const noexcept
  {
    return positions_;
  }

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
      positions_[value_index(entry.value)] = kMissingPosition;
    }
    heap_.clear();
  }

  void reserve_entries(const std::size_t capacity)
  {
    if (capacity > max_heap_size()) {
      throw std::length_error("indexed heap capacity exceeds 32-bit positions");
    }
    heap_.reserve(capacity);
  }

  void reserve(const std::size_t capacity)
  {
    reserve_entries(capacity);
  }

  void reserve_values(const std::size_t value_count)
  {
    if (value_count > positions_.size()) {
      positions_.resize(value_count, kMissingPosition);
    }
  }

  void prepare(const std::size_t value_count)
  {
    reserve_entries(value_count);
    reserve_values(value_count);
  }

  void update(const Value value, const float key)
  {
    const std::size_t index = value_index(value);
    if (index >= positions_.size() || positions_[index] == kMissingPosition) {
      insert_at_index(value, key, index);
      return;
    }

    const std::size_t position = decode_position(positions_[index]);
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
    if (index >= positions_.size() || positions_[index] == kMissingPosition) {
      return false;
    }

    std::size_t position = decode_position(positions_[index]);
    if (position > 0) {
      const Entry entry = heap_[position];
      do {
        const std::size_t parent_position = parent(position);
        heap_[position] = heap_[parent_position];
        set_position(heap_[position].value, position);
        position = parent_position;
      } while (position > 0);
      heap_.front() = entry;
      positions_[index] = encode_position(0);
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
    if (positions_[index] != kMissingPosition) {
      throw std::runtime_error("duplicate indexed heap insertion");
    }
    if (heap_.size() >= max_heap_size()) {
      throw std::length_error("indexed heap exceeds 32-bit positions");
    }

    const std::size_t position = heap_.size();
    heap_.push_back({key, value});
    positions_[index] = encode_position(position);
    sift_up(position);
  }

  Entry pop_min()
  {
    const Entry result = heap_.front();
    if (heap_.size() > 1) {
      heap_.front() = heap_.back();
      set_position(heap_.front().value, 0);
    }
    heap_.pop_back();
    positions_[value_index(result.value)] = kMissingPosition;
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
      set_position(heap_[position].value, position);
      position = parent_position;
      if (position == 0) {
        break;
      }
      parent_position = parent(position);
    } while (!cost_less(heap_[parent_position], entry));

    heap_[position] = entry;
    set_position(entry.value, position);
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
      set_position(heap_[position].value, position);
      position = smallest;
      smallest = sift_down_child(position, entry, heap_size);
    } while (smallest != position);

    heap_[position] = entry;
    set_position(entry.value, position);
  }

  std::size_t max_heap_size() const noexcept
  {
    return tagged_positions_ ?
               static_cast<std::size_t>(kMissingPosition >> tag_bits_) + 1 :
               static_cast<std::size_t>(kMissingPosition);
  }

  Position encode_position(const std::size_t position) const
  {
    if (position >= max_heap_size()) {
      throw std::length_error("indexed heap position exceeds tagged capacity");
    }
    if (!tagged_positions_) {
      return static_cast<Position>(position);
    }
    return static_cast<Position>(
        (static_cast<Position>(position) << tag_bits_) | (heap_tag_ + 1));
  }

  std::size_t decode_position(const Position encoded) const
  {
    if (encoded == kMissingPosition) {
      throw std::runtime_error("indexed heap value is missing");
    }
    if (!tagged_positions_) {
      const std::size_t position = encoded;
      if (position >= heap_.size()) {
        throw std::runtime_error("indexed heap position is out of range");
      }
      return position;
    }
    if ((encoded & tag_mask_) != heap_tag_ + 1) {
      throw std::runtime_error("indexed heap value belongs to another heap");
    }
    const std::size_t position = encoded >> tag_bits_;
    if (position >= heap_.size()) {
      throw std::runtime_error("indexed heap position is out of range");
    }
    return position;
  }

  void set_position(const Value value, const std::size_t position)
  {
    positions_[value_index(value)] = encode_position(position);
  }

  std::vector<Entry> heap_;
  PositionStorage owned_positions_;
  PositionStorage &positions_;
  Position heap_tag_ = 0;
  Position tag_bits_ = 0;
  Position tag_mask_ = 0;
  bool tagged_positions_ = false;
};

}  // namespace standalone_decimator
