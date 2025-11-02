#include <gtest/gtest.h>
#include "block.hpp"

using namespace tlsf::detail;


TEST(UtilityTests, voidPtrConversionIsReversible){
    block_header* header = new block_header;
    header->set_size(128);
    
    EXPECT_EQ(header, block_header::from_void_ptr(header->to_void_ptr()));
}

TEST(UtilityTests, alignPtrCorrect){
    size_t align = 32;
    void* ptr = (void*) 1032;
    void* aligned_ptr = (void*) 1056; //must be a multiple of 32
    auto result = align_ptr(ptr, align);
    EXPECT_EQ(result, aligned_ptr);
}

// ============================================================================
// block_split Tests
// ============================================================================

TEST(BlockSplitTests, SplitCreatesCorrectPointerOffsets) {
    // Allocate a buffer to work with
    constexpr size_t buffer_size = 1024;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    // Initialize a block header at the start of the buffer
    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    block->set_size(512); // Total block size
    block->set_used(); // Mark as used initially

    // Split the block: keep 256 bytes, split off the rest
    constexpr size_t keep_size = 256;
    block_header* remaining = block_split(block, keep_size);

    // Verify the original block's size is updated
    EXPECT_EQ(block->get_size(), keep_size);

    // Verify the remaining block starts at the correct memory address
    // Expected address: block's data pointer + keep_size - BLOCK_HEADER_OVERHEAD
    uint8_t* expected_remaining_addr = reinterpret_cast<uint8_t*>(block->to_void_ptr())
                                       + keep_size - BLOCK_HEADER_OVERHEAD;
    EXPECT_EQ(reinterpret_cast<uint8_t*>(remaining), expected_remaining_addr);

    // Verify the remaining block's size
    // Original size (512) - keep_size (256) - BLOCK_HEADER_OVERHEAD
    size_t expected_remaining_size = 512 - keep_size - BLOCK_HEADER_OVERHEAD;
    EXPECT_EQ(remaining->get_size(), expected_remaining_size);

    // Verify the remaining block is marked as free
    EXPECT_TRUE(remaining->is_free());
}

TEST(BlockSplitTests, SplitMaintainsProperAlignment) {
    constexpr size_t buffer_size = 1024;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    block->set_size(600);

    block_header* remaining = block_split(block, 256);

    // Verify remaining block's data pointer is properly aligned
    void* remaining_data = remaining->to_void_ptr();
    uintptr_t addr = reinterpret_cast<uintptr_t>(remaining_data);
    EXPECT_EQ(addr % ALIGN_SIZE, 0) << "Remaining block data not aligned to " << ALIGN_SIZE << " bytes";
}

TEST(BlockSplitTests, SplitPreservesTotalMemoryAccountability) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    const size_t original_size = 1000;
    block->set_size(original_size);

    const size_t split_size = 400;
    block_header* remaining = block_split(block, split_size);

    // Total memory should be conserved:
    // original_size = block->get_size() + remaining->get_size() + BLOCK_HEADER_OVERHEAD
    EXPECT_EQ(original_size, block->get_size() + remaining->get_size() + BLOCK_HEADER_OVERHEAD);
}

TEST(BlockSplitTests, SplitWithMinimumSizedBlock) {
    constexpr size_t buffer_size = 1024;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    // Set up a block that when split will create a minimum-sized remaining block
    block->set_size(BLOCK_SIZE_MIN * 2 + BLOCK_HEADER_OVERHEAD);

    block_header* remaining = block_split(block, BLOCK_SIZE_MIN);

    EXPECT_EQ(block->get_size(), BLOCK_SIZE_MIN);
    EXPECT_GE(remaining->get_size(), BLOCK_SIZE_MIN);
}

TEST(BlockSplitTests, ConsecutiveSplitsProduceCorrectOffsets) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    block->set_size(1024);

    // First split: keep 256, split off rest
    block_header* remaining1 = block_split(block, 256);
    size_t remaining1_addr = reinterpret_cast<size_t>(remaining1);

    // Second split on the remaining block: keep 256, split off rest again
    block_header* remaining2 = block_split(remaining1, 256);
    size_t remaining2_addr = reinterpret_cast<size_t>(remaining2);

    // Verify the second remaining block is after the first
    EXPECT_GT(remaining2_addr, remaining1_addr);

    // Verify the exact offset
    // The offset should be the size of remaining1's data (256) plus the header overhead (8)
    size_t expected_offset = 256 + BLOCK_HEADER_OVERHEAD;
    EXPECT_EQ(remaining2_addr - remaining1_addr, expected_offset);
}

// ============================================================================
// block_coalesce Tests
// ============================================================================

TEST(BlockCoalesceTests, CoalesceCombinesSizesCorrectly) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    // Set up two adjacent blocks
    block_header* block1 = reinterpret_cast<block_header*>(buffer);
    block1->size = 0;
    block1->set_size(256);
    block1->set_free();

    // Calculate where block2 should start
    uint8_t* block2_addr = reinterpret_cast<uint8_t*>(block1->to_void_ptr()) + 256;
    block_header* block2 = reinterpret_cast<block_header*>(block2_addr);
    block2->size = 0;
    block2->set_size(256);
    block2->set_free();
    block2->prev_phys_block = block1;

    // Coalesce the blocks
    block_header* coalesced = block_coalesce(block1, block2);

    // Verify the coalesced block is at the same address as block1
    EXPECT_EQ(coalesced, block1);

    // Verify the size is the sum of both blocks plus one header overhead
    // block1->size (256) + block2->size (256) + BLOCK_HEADER_OVERHEAD
    size_t expected_size = 256 + 256 + BLOCK_HEADER_OVERHEAD;
    EXPECT_EQ(coalesced->get_size(), expected_size);
}

TEST(BlockCoalesceTests, CoalescePreservesFirstBlockAddress) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block1 = reinterpret_cast<block_header*>(buffer);
    block1->size = 0;
    block1->set_size(512);

    uint8_t* block2_addr = reinterpret_cast<uint8_t*>(block1->to_void_ptr()) + 512;
    block_header* block2 = reinterpret_cast<block_header*>(block2_addr);
    block2->size = 0;
    block2->set_size(512);
    block2->prev_phys_block = block1;

    uintptr_t original_block1_addr = reinterpret_cast<uintptr_t>(block1);

    block_header* coalesced = block_coalesce(block1, block2);

    // The coalesced block must be at the same address as the first block
    EXPECT_EQ(reinterpret_cast<uintptr_t>(coalesced), original_block1_addr);
}

TEST(BlockCoalesceTests, CoalesceWithDifferentSizedBlocks) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block1 = reinterpret_cast<block_header*>(buffer);
    block1->size = 0;
    const size_t size1 = 128;
    block1->set_size(size1);

    uint8_t* block2_addr = reinterpret_cast<uint8_t*>(block1->to_void_ptr()) + size1;
    block_header* block2 = reinterpret_cast<block_header*>(block2_addr);
    block2->size = 0;
    const size_t size2 = 384;
    block2->set_size(size2);
    block2->prev_phys_block = block1;

    block_header* coalesced = block_coalesce(block1, block2);

    size_t expected_size = size1 + size2 + BLOCK_HEADER_OVERHEAD;
    EXPECT_EQ(coalesced->get_size(), expected_size);
}

TEST(BlockCoalesceTests, CoalesceMaintainsNextBlockLink) {
    constexpr size_t buffer_size = 3072;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    // Set up three blocks: block1, block2 (to be coalesced), and block3 (next)
    block_header* block1 = reinterpret_cast<block_header*>(buffer);
    block1->size = 0;
    block1->set_size(256);

    // block2 starts at block1's data + 256 bytes
    uint8_t* block2_addr = reinterpret_cast<uint8_t*>(block1->to_void_ptr()) + 256;
    block_header* block2 = reinterpret_cast<block_header*>(block2_addr);
    block2->size = 0;
    block2->set_size(256);
    block2->prev_phys_block = block1;

    // block3 starts at block2's data + 256 bytes
    uint8_t* block3_addr = reinterpret_cast<uint8_t*>(block2->to_void_ptr()) + 256;
    block_header* block3 = reinterpret_cast<block_header*>(block3_addr);
    block3->size = 0;
    block3->set_size(256);
    block3->prev_phys_block = block2;

    // Coalesce block1 and block2
    block_header* coalesced = block_coalesce(block1, block2);

    // Verify that block3's prev_phys_block is updated to point to the coalesced block
    // Note: block_coalesce calls link_next() which updates the next block's prev_phys_block
    // The next block after coalescing should be at: coalesced->to_void_ptr() + coalesced->get_size() - BLOCK_HEADER_OVERHEAD
    block_header* expected_next = coalesced->get_next();
    EXPECT_EQ(expected_next->prev_phys_block, coalesced);
}

// ============================================================================
// Integration Tests: Split and Coalesce
// ============================================================================

TEST(BlockIntegrationTests, SplitThenCoalesceRestoresOriginal) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* original = reinterpret_cast<block_header*>(buffer);
    original->size = 0;
    const size_t original_size = 1000;
    original->set_size(original_size);

    // Split the block
    const size_t split_size = 400;
    block_header* remaining = block_split(original, split_size);

    // Now coalesce them back together
    block_header* coalesced = block_coalesce(original, remaining);

    // The size should be restored (accounting for the extra header overhead from split)
    EXPECT_EQ(coalesced->get_size(), original_size);
    EXPECT_EQ(coalesced, original);
}

TEST(BlockIntegrationTests, MultipleCoalesceOperations) {
    constexpr size_t buffer_size = 4096;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    // Create four adjacent blocks of equal size
    const size_t block_size = 256;
    block_header* blocks[4];

    for (int i = 0; i < 4; ++i) {
        size_t offset = i * (block_size + BLOCK_HEADER_OVERHEAD);
        blocks[i] = reinterpret_cast<block_header*>(buffer + offset);
        blocks[i]->size = 0;
        blocks[i]->set_size(block_size);
        if (i > 0) {
            blocks[i]->prev_phys_block = blocks[i-1];
        }
    }

    // Coalesce blocks[0] and blocks[1]
    block_header* coalesced1 = block_coalesce(blocks[0], blocks[1]);
    EXPECT_EQ(coalesced1->get_size(), 2 * block_size + BLOCK_HEADER_OVERHEAD);

    // Update blocks[2]'s prev pointer manually (normally done by link_next)
    blocks[2]->prev_phys_block = coalesced1;

    // Coalesce the result with blocks[2]
    block_header* coalesced2 = block_coalesce(coalesced1, blocks[2]);
    EXPECT_EQ(coalesced2->get_size(), 3 * block_size + 2 * BLOCK_HEADER_OVERHEAD);

    // Update blocks[3]'s prev pointer
    blocks[3]->prev_phys_block = coalesced2;

    // Finally coalesce with blocks[3]
    block_header* final_block = block_coalesce(coalesced2, blocks[3]);
    EXPECT_EQ(final_block->get_size(), 4 * block_size + 3 * BLOCK_HEADER_OVERHEAD);
    EXPECT_EQ(final_block, blocks[0]); // Should still be at original address
}

// ============================================================================
// Pointer Arithmetic Edge Cases and Verification
// ============================================================================

TEST(BlockPointerTests, GetNextReturnsCorrectAddress) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    const size_t block_size = 512;
    block->set_size(block_size);

    // Set up a next block
    // get_next() uses: offset_to_block(to_void_ptr(), get_size() - BLOCK_HEADER_OVERHEAD)
    // So the next block is at: to_void_ptr() + (get_size() - BLOCK_HEADER_OVERHEAD)
    uint8_t* next_block_addr = reinterpret_cast<uint8_t*>(block->to_void_ptr()) + block_size - BLOCK_HEADER_OVERHEAD;
    block_header* expected_next = reinterpret_cast<block_header*>(next_block_addr);
    expected_next->size = 0;
    expected_next->set_size(256);
    expected_next->prev_phys_block = block;

    // Get the next block using the method
    block_header* actual_next = block->get_next();

    EXPECT_EQ(actual_next, expected_next);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(actual_next), reinterpret_cast<uintptr_t>(expected_next));
}

TEST(BlockPointerTests, LinkNextUpdatesPointerCorrectly) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    block->set_size(512);

    // link_next() calls get_next() which uses: to_void_ptr() + (get_size() - BLOCK_HEADER_OVERHEAD)
    uint8_t* next_block_addr = reinterpret_cast<uint8_t*>(block->to_void_ptr()) + 512 - BLOCK_HEADER_OVERHEAD;
    block_header* next_block = reinterpret_cast<block_header*>(next_block_addr);
    next_block->size = 0;
    next_block->set_size(256);
    next_block->prev_phys_block = nullptr; // Initially unlinked

    // Link the next block
    block_header* linked = block->link_next();

    // Verify the link was established
    EXPECT_EQ(linked, next_block);
    EXPECT_EQ(next_block->prev_phys_block, block);
}

TEST(BlockPointerTests, OffsetToBlockCalculatesCorrectAddress) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    void* base_ptr = buffer;
    const size_t offset = 256;

    block_header* offset_block = block_header::offset_to_block(base_ptr, offset);

    // Expected address is base_ptr + offset
    uint8_t* expected_addr = reinterpret_cast<uint8_t*>(base_ptr) + offset;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(offset_block), reinterpret_cast<uintptr_t>(expected_addr));
}

TEST(BlockPointerTests, FromVoidPtrAndToVoidPtrAreInverses) {
    constexpr size_t buffer_size = 1024;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    block->set_size(256);

    // Convert to void pointer and back
    void* data_ptr = block->to_void_ptr();
    block_header* recovered = block_header::from_void_ptr(data_ptr);

    EXPECT_EQ(recovered, block);
}

TEST(BlockPointerTests, ToVoidPtrPointsPastHeader) {
    constexpr size_t buffer_size = 1024;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    block->set_size(256);

    void* data_ptr = block->to_void_ptr();

    // The data pointer should be exactly BLOCK_START_OFFSET bytes past the block header
    uintptr_t expected_addr = reinterpret_cast<uintptr_t>(block) + BLOCK_START_OFFSET;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(data_ptr), expected_addr);
}

TEST(BlockPointerTests, BlockHeaderOverheadMatchesActualSize) {
    // This test verifies that BLOCK_HEADER_OVERHEAD is correctly defined
    // BLOCK_HEADER_OVERHEAD should equal sizeof(size_t)
    EXPECT_EQ(BLOCK_HEADER_OVERHEAD, sizeof(std::size_t));
}

TEST(BlockPointerTests, BlockStartOffsetIsCorrect) {
    // Verify that BLOCK_START_OFFSET correctly accounts for prev_phys_block and size fields
    size_t expected_offset = offsetof(block_header, size) + sizeof(std::size_t);
    EXPECT_EQ(BLOCK_START_OFFSET, expected_offset);
}

// ============================================================================
// Block Status and Flag Tests
// ============================================================================

TEST(BlockStatusTests, SplitMarksRemainingAsFree) {
    constexpr size_t buffer_size = 1024;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    block->set_size(512);
    block->set_used();

    block_header* remaining = block_split(block, 256);

    // The remaining block should be marked as free
    EXPECT_TRUE(remaining->is_free());
    // The original block's status should be preserved
    EXPECT_FALSE(block->is_free());
}

TEST(BlockStatusTests, MarkAsFreeUpdatesNextBlock) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    block->set_size(512);
    block->set_used();

    // next_block is at: block->to_void_ptr() + (block->get_size() - BLOCK_HEADER_OVERHEAD)
    uint8_t* next_block_addr = reinterpret_cast<uint8_t*>(block->to_void_ptr()) + 512 - BLOCK_HEADER_OVERHEAD;
    block_header* next_block = reinterpret_cast<block_header*>(next_block_addr);
    next_block->size = 0;
    next_block->set_size(256);
    next_block->prev_phys_block = block;
    next_block->set_prev_used();

    // Mark block as free
    block->mark_as_free();

    // Verify both the block and next block are updated
    EXPECT_TRUE(block->is_free());
    EXPECT_TRUE(next_block->is_prev_free());
}

TEST(BlockStatusTests, MarkAsUsedUpdatesNextBlock) {
    constexpr size_t buffer_size = 2048;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    block->set_size(512);
    block->set_free();

    // next_block is at: block->to_void_ptr() + (block->get_size() - BLOCK_HEADER_OVERHEAD)
    uint8_t* next_block_addr = reinterpret_cast<uint8_t*>(block->to_void_ptr()) + 512 - BLOCK_HEADER_OVERHEAD;
    block_header* next_block = reinterpret_cast<block_header*>(next_block_addr);
    next_block->size = 0;
    next_block->set_size(256);
    next_block->prev_phys_block = block;
    next_block->set_prev_free();

    // Mark block as used
    block->mark_as_used();

    // Verify both the block and next block are updated
    EXPECT_FALSE(block->is_free());
    EXPECT_FALSE(next_block->is_prev_free());
}

TEST(BlockStatusTests, SetSizePreservesFlags) {
    constexpr size_t buffer_size = 1024;
    alignas(ALIGN_SIZE) uint8_t buffer[buffer_size];

    block_header* block = reinterpret_cast<block_header*>(buffer);
    block->size = 0;
    block->set_size(256);
    block->set_free();
    block->set_prev_free();

    // Change the size
    block->set_size(512);

    // Flags should be preserved
    EXPECT_TRUE(block->is_free());
    EXPECT_TRUE(block->is_prev_free());
    EXPECT_EQ(block->get_size(), 512);
}
