// The memory map's slot arithmetic (core/libs/d3d12_xs/gpu_memory.h).
#include "core/libs/d3d12_xs/gpu_memory.h"

#include "tests/unit/test.h"

using namespace holy::d3d12;

TEST(memory_slots_cover_a_heap_with_overlapping_views) {
  EXPECT(slot_count(0x10000) == 1);
  EXPECT(slot_count(kSlotBytes) == 1);
  EXPECT(slot_count(kSlotBytes + 0x10000) == 2);
  // 1 GiB: slots at 0, one stride and two strides (the last 128 KiB).
  EXPECT(slot_count(1ull << 30) == 3);
  // The last slot's view starts inside the heap and holds its last page.
  const uint64_t heap = 1ull << 30;
  EXPECT((slot_count(heap) - 1) * kSlotStride < heap);
  EXPECT((slot_count(heap) - 1) * kSlotStride + kSlotBytes >= heap);
}

TEST(pages_lie_wholly_in_the_slot_their_start_selects) {
  uint64_t slot = 0;
  uint32_t offset = 0;
  page_slot(0, &slot, &offset);
  EXPECT(slot == 0 && offset == 0);
  page_slot(0x12340000, &slot, &offset);
  EXPECT(slot == 0 && offset == 0x12340000u);
  // The last page before a slot boundary stays in the first slot, wholly.
  page_slot(static_cast<int64_t>(kSlotStride) - 0x10000, &slot, &offset);
  EXPECT(slot == 0 && offset + 0x10000ull <= kSlotBytes);
  page_slot(static_cast<int64_t>(kSlotStride), &slot, &offset);
  EXPECT(slot == 1 && offset == 0);
  // A page that starts 0x1000 bytes into the next stride.
  page_slot(static_cast<int64_t>(kSlotStride) + 0x1000, &slot, &offset);
  EXPECT(slot == 1 && offset == 0x1000u);
  // Heaps need not start on a page boundary: the first page starts before
  // the heap, and its view offset wraps (addresses before the heap fall
  // outside the view; those in it land at their heap offset).
  page_slot(-0x3000, &slot, &offset);
  EXPECT(slot == 0 && offset == 0xFFFFD000u);
  EXPECT(static_cast<uint32_t>(offset + 0x3000u) == 0);
  EXPECT(static_cast<uint32_t>(offset + 0x3010u) == 0x10u);
}
