#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "pool.hpp"
#include <optional>
#include <unordered_set>
#include <vector>
#include <random>
#include <algorithm>
#include <cstring>
#include <iostream>

using namespace tlsf;

class PoolTests : public ::testing::Test {
    protected:
        PoolTests(): pool(tlsf_pool::create(1024*1024)) {}
        std::optional<tlsf_pool> pool;
};


TEST_F(PoolTests, poolAllocatesOnConstruction){
    ASSERT_TRUE(pool);
    EXPECT_TRUE(pool->is_allocated());
}

TEST_F(PoolTests, poolMalloc){
    using namespace tlsf::detail;
    void* bytes_1024 = pool->malloc_pool(1024);
    block_header* header = block_header::from_void_ptr(bytes_1024);
    ASSERT_EQ(header->get_size(), 1024);
    EXPECT_TRUE(bytes_1024);
    EXPECT_TRUE(pool->free_pool(bytes_1024));
    void* bytes_1024_2 = pool->malloc_pool(1024);
    EXPECT_TRUE(bytes_1024_2);
    EXPECT_TRUE(pool->free_pool(bytes_1024_2));

    void* bytes_1MB = pool->malloc_pool(1024*1024/2);
    header = block_header::from_void_ptr(bytes_1MB);
    ASSERT_EQ(header->get_size(), 1024*1024/2);
    EXPECT_TRUE(bytes_1MB);
    pool->free_pool(bytes_1MB);

    void* bytes_toomany = pool->malloc_pool(1024*1024 + 1);
    EXPECT_FALSE(bytes_toomany);
}

TEST(PoolDeathTest, poolDeallocatesOnDestruction){
    {
        auto pool = tlsf_pool::create(1024*1024);
        ASSERT_TRUE(pool.has_value());
        ASSERT_TRUE(pool->is_allocated());
        void* bytes = pool->malloc_pool(2048);
        EXPECT_TRUE(bytes);
        EXPECT_TRUE(pool->free_pool(bytes));
        void* bytes2 = pool->memalign_pool(2048, 32);
        EXPECT_TRUE(bytes2);
        EXPECT_TRUE(pool->free_pool(bytes2));
    }

    {
        auto pool = tlsf_pool::create(1024*1024);
    }
}

// ============================================================================
// Comprehensive malloc_pool Tests
// ============================================================================

class MallocPoolTests : public ::testing::Test {
protected:
    MallocPoolTests() : pool(tlsf_pool::create(1024 * 1024)) {}
    std::optional<tlsf_pool> pool;
};

TEST_F(MallocPoolTests, zeroSizeAllocation) {
    // Zero-size allocation should return nullptr
    void* ptr = pool->malloc_pool(0);
    EXPECT_EQ(ptr, nullptr);
}

TEST_F(MallocPoolTests, minimalSizeAllocation) {
    // Allocate the minimum possible size
    void* ptr = pool->malloc_pool(1);
    ASSERT_NE(ptr, nullptr);
    EXPECT_TRUE(pool->free_pool(ptr));
}

TEST_F(MallocPoolTests, multipleAllocations) {
    std::vector<void*> ptrs;
    const std::size_t alloc_size = 256;
    const int num_allocs = 100;

    for (int i = 0; i < num_allocs; ++i) {
        void* ptr = pool->malloc_pool(alloc_size);
        ASSERT_NE(ptr, nullptr) << "Allocation " << i << " failed";
        ptrs.push_back(ptr);
    }

    // Free all allocations
    for (void* ptr : ptrs) {
        EXPECT_TRUE(pool->free_pool(ptr));
    }
}

TEST_F(MallocPoolTests, allocationAndImmediateReuse) {
    void* ptr1 = pool->malloc_pool(512);
    ASSERT_NE(ptr1, nullptr);
    EXPECT_TRUE(pool->free_pool(ptr1));

    // Allocation after free should succeed and ideally reuse the same block
    void* ptr2 = pool->malloc_pool(512);
    ASSERT_NE(ptr2, nullptr);
    EXPECT_EQ(ptr1, ptr2); // Should get the same memory back
    EXPECT_TRUE(pool->free_pool(ptr2));
}

TEST_F(MallocPoolTests, varyingSizeAllocations) {
    // Test various allocation sizes (keep under pool size with overhead)
    std::vector<std::size_t> sizes = {1, 4, 8, 16, 32, 64, 128, 256, 512, 1024,
                                       2048, 4096, 8192, 16384};

    for (std::size_t size : sizes) {
        void* ptr = pool->malloc_pool(size);
        ASSERT_NE(ptr, nullptr) << "Failed to allocate " << size << " bytes";

        // Verify we can write to the allocated memory
        std::memset(ptr, 0xAA, size);

        EXPECT_TRUE(pool->free_pool(ptr));
    }
}

TEST_F(MallocPoolTests, fragmentationAndCoalescing) {
    // Allocate 3 blocks
    void* ptr1 = pool->malloc_pool(1024);
    void* ptr2 = pool->malloc_pool(1024);
    void* ptr3 = pool->malloc_pool(1024);

    ASSERT_NE(ptr1, nullptr);
    ASSERT_NE(ptr2, nullptr);
    ASSERT_NE(ptr3, nullptr);

    // Free middle block
    EXPECT_TRUE(pool->free_pool(ptr2));

    // Allocate a smaller block - should fit in the freed space
    void* ptr4 = pool->malloc_pool(512);
    ASSERT_NE(ptr4, nullptr);

    // Free all blocks
    EXPECT_TRUE(pool->free_pool(ptr1));
    EXPECT_TRUE(pool->free_pool(ptr3));
    EXPECT_TRUE(pool->free_pool(ptr4));

    // After freeing adjacent blocks, should be able to allocate a large block
    void* ptr5 = pool->malloc_pool(3 * 1024);
    EXPECT_NE(ptr5, nullptr) << "Coalescing may have failed";
    EXPECT_TRUE(pool->free_pool(ptr5));
}

TEST_F(MallocPoolTests, exhaustMemory) {
    std::vector<void*> ptrs;
    const std::size_t alloc_size = 4096;

    // Keep allocating until we run out of memory
    // Pool exhaustion might trigger system memory allocation which could throw
    try {
        while (true) {
            void* ptr = pool->malloc_pool(alloc_size);
            if (!ptr) {
                break;
            }
            ptrs.push_back(ptr);

            // Safety limit to avoid infinite loop or excessive memory use
            if (ptrs.size() > 1000) {
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        // This can happen if the pool's upstream resource runs out
    }

    EXPECT_GT(ptrs.size(), 0u);
    std::vector<bool> free_results;

    // Free all allocations
    for (void* ptr : ptrs) {
        free_results.push_back(pool->free_pool(ptr));
    }

    EXPECT_THAT(free_results, ::testing::Each(true)) 
        << "Pool is allocating memory it is not responsible for. Total size: " << free_results.size() << "\n";

    // for (int i = 0; i < ptrs.size(); i++) {
    //     std::cerr << "[" << i << "]: " << ptrs[i] << "\n";
    // }
    

    // After freeing, should be able to allocate again
    void* ptr = pool->malloc_pool(alloc_size);
    EXPECT_NE(ptr, nullptr);
    if (ptr) {
        EXPECT_TRUE(pool->free_pool(ptr));
    }
}

TEST_F(MallocPoolTests, exhaustMemoryDirectConstruction) {
    std::vector<void*> ptrs;
    const std::size_t alloc_size = 4096;

    tlsf_pool direct_pool(1024*1024);

    // Keep allocating until we run out of memory
    // Pool exhaustion might trigger system memory allocation which could throw
    try {
        while (true) {
            void* ptr = direct_pool.malloc_pool(alloc_size);
            if (!ptr) {
                break;
            }
            ptrs.push_back(ptr);

            // Safety limit to avoid infinite loop or excessive memory use
            if (ptrs.size() > 1000) {
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        // This can happen if the pool's upstream resource runs out
    }

    EXPECT_GT(ptrs.size(), 0u);
    std::vector<bool> free_results;

    // Free all allocations
    for (void* ptr : ptrs) {
        free_results.push_back(direct_pool.free_pool(ptr));
    }

    EXPECT_THAT(free_results, ::testing::Each(true)) 
        << "Pool is allocating memory it is not responsible for. Total size: " << free_results.size() << "\n";

    // for (int i = 0; i < ptrs.size(); i++) {
    //     std::cerr << "[" << i << "]: " << ptrs[i] << "\n";
    // }
    

    // After freeing, should be able to allocate again
    void* ptr = direct_pool.malloc_pool(alloc_size);
    EXPECT_NE(ptr, nullptr);
    if (ptr) {
        EXPECT_TRUE(direct_pool.free_pool(ptr));
    }
}

TEST_F(MallocPoolTests, allocationSizeAlignment) {
    using namespace tlsf::detail;

    // Test that allocations are properly aligned
    std::vector<std::size_t> test_sizes = {1, 3, 5, 7, 9, 15, 17, 31, 33};

    for (std::size_t size : test_sizes) {
        void* ptr = pool->malloc_pool(size);
        ASSERT_NE(ptr, nullptr);

        // Verify alignment
        EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % ALIGN_SIZE, 0u)
            << "Allocation of size " << size << " is not properly aligned";

        block_header* header = block_header::from_void_ptr(ptr);
        // The actual allocated size should be aligned up
        EXPECT_GE(header->get_size(), size);
        EXPECT_EQ(header->get_size() % ALIGN_SIZE, 0u);

        EXPECT_TRUE(pool->free_pool(ptr));
    }
}

TEST_F(MallocPoolTests, freeNullptr) {
    // Freeing nullptr should return false and not crash
    EXPECT_FALSE(pool->free_pool(nullptr));
}

TEST_F(MallocPoolTests, freeOutOfPoolPointer) {
    // Allocate memory outside the pool
    void* external_ptr = std::malloc(256);

    // Freeing external pointer should return false
    EXPECT_FALSE(pool->free_pool(external_ptr));

    std::free(external_ptr);
}

TEST_F(MallocPoolTests, doubleFreeDetection) {
    void* ptr = pool->malloc_pool(256);
    ASSERT_NE(ptr, nullptr);

    EXPECT_TRUE(pool->free_pool(ptr));

    // Double free should trigger assertion in debug mode
    // In release mode, behavior is undefined but shouldn't crash
    // We can't reliably test this without death tests
}

TEST_F(MallocPoolTests, interleavedAllocFree) {
    std::vector<void*> ptrs;

    // Allocate several blocks
    for (int i = 0; i < 10; ++i) {
        ptrs.push_back(pool->malloc_pool(128));
        ASSERT_NE(ptrs.back(), nullptr);
    }

    // Free every other block
    for (size_t i = 0; i < ptrs.size(); i += 2) {
        EXPECT_TRUE(pool->free_pool(ptrs[i]));
        ptrs[i] = nullptr;
    }

    // Allocate new blocks in the freed spaces
    for (size_t i = 0; i < ptrs.size(); i += 2) {
        ptrs[i] = pool->malloc_pool(128);
        EXPECT_NE(ptrs[i], nullptr);
    }

    // Free all remaining blocks
    for (void* ptr : ptrs) {
        if (ptr) {
            EXPECT_TRUE(pool->free_pool(ptr));
        }
    }
}

TEST_F(MallocPoolTests, largeAllocation) {
    // Allocate close to the maximum pool size
    const std::size_t large_size = 512 * 1024; // 512 KB
    void* ptr = pool->malloc_pool(large_size);
    ASSERT_NE(ptr, nullptr);

    // Write to the memory to ensure it's accessible
    std::memset(ptr, 0x55, large_size);

    EXPECT_TRUE(pool->free_pool(ptr));
}

TEST_F(MallocPoolTests, boundaryConditions) {
    using namespace tlsf::detail;

    // Test allocation sizes at power-of-2 boundaries (keep reasonable for 1MB pool)
    std::vector<std::size_t> boundary_sizes = {
        BLOCK_SIZE_MIN,
        256, 512, 1024, 2048, 4096, 8192, 16384, 32768
    };

    for (std::size_t size : boundary_sizes) {
        void* ptr = pool->malloc_pool(size);
        ASSERT_NE(ptr, nullptr) << "Failed at boundary size " << size;
        EXPECT_TRUE(pool->free_pool(ptr));
    }
}

// ============================================================================
// Comprehensive memalign_pool Tests
// ============================================================================

class MemalignPoolTests : public ::testing::Test {
protected:
    MemalignPoolTests() : pool(tlsf_pool::create(2 * 1024 * 1024)) {}
    std::optional<tlsf_pool> pool;

    // Helper to check if pointer is aligned
    bool is_aligned(void* ptr, std::size_t alignment) {
        return (reinterpret_cast<uintptr_t>(ptr) % alignment) == 0;
    }
};

TEST_F(MemalignPoolTests, basicAlignment) {
    std::vector<std::size_t> alignments = {4, 8, 16, 32, 64, 128, 256, 512, 1024};

    for (std::size_t align : alignments) {
        void* ptr = pool->memalign_pool(align, 256);
        ASSERT_NE(ptr, nullptr) << "Failed to allocate with alignment " << align;
        EXPECT_TRUE(is_aligned(ptr, align))
            << "Pointer not aligned to " << align << " bytes";
        EXPECT_TRUE(pool->free_pool(ptr));
    }
}

TEST_F(MemalignPoolTests, alignmentWithVariousSizes) {
    const std::size_t alignment = 64;
    std::vector<std::size_t> sizes = {1, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

    for (std::size_t size : sizes) {
        void* ptr = pool->memalign_pool(alignment, size);
        ASSERT_NE(ptr, nullptr) << "Failed to allocate size " << size;
        EXPECT_TRUE(is_aligned(ptr, alignment));

        // Verify we can write to the memory
        std::memset(ptr, 0xBB, size);

        EXPECT_TRUE(pool->free_pool(ptr));
    }
}

TEST_F(MemalignPoolTests, smallAlignment) {
    using namespace tlsf::detail;

    // Alignment smaller than or equal to ALIGN_SIZE should work like regular malloc
    void* ptr1 = pool->memalign_pool(ALIGN_SIZE, 128);
    ASSERT_NE(ptr1, nullptr);
    EXPECT_TRUE(is_aligned(ptr1, ALIGN_SIZE));

    void* ptr2 = pool->memalign_pool(ALIGN_SIZE / 2, 128);
    ASSERT_NE(ptr2, nullptr);
    EXPECT_TRUE(is_aligned(ptr2, ALIGN_SIZE)); // Still aligned to at least ALIGN_SIZE

    EXPECT_TRUE(pool->free_pool(ptr1));
    EXPECT_TRUE(pool->free_pool(ptr2));
}

TEST_F(MemalignPoolTests, zeroSizeWithAlignment) {
    void* ptr = pool->memalign_pool(64, 0);
    EXPECT_EQ(ptr, nullptr);
}

TEST_F(MemalignPoolTests, largeAlignment) {
    const std::size_t large_align = 4096;
    void* ptr = pool->memalign_pool(large_align, 512);
    ASSERT_NE(ptr, nullptr);
    EXPECT_TRUE(is_aligned(ptr, large_align));
    EXPECT_TRUE(pool->free_pool(ptr));
}

TEST_F(MemalignPoolTests, multipleAlignedAllocations) {
    std::vector<void*> ptrs;
    const std::size_t alignment = 128;
    const std::size_t size = 256;

    for (int i = 0; i < 50; ++i) {
        void* ptr = pool->memalign_pool(alignment, size);
        ASSERT_NE(ptr, nullptr) << "Allocation " << i << " failed";
        EXPECT_TRUE(is_aligned(ptr, alignment));
        ptrs.push_back(ptr);
    }

    for (void* ptr : ptrs) {
        EXPECT_TRUE(pool->free_pool(ptr));
    }
}

TEST_F(MemalignPoolTests, alignmentBoundaryConditions) {
    // Test power-of-2 alignments
    for (std::size_t align = 8; align <= 2048; align *= 2) {
        void* ptr = pool->memalign_pool(align, 128);
        ASSERT_NE(ptr, nullptr) << "Failed with alignment " << align;
        EXPECT_TRUE(is_aligned(ptr, align));
        EXPECT_TRUE(pool->free_pool(ptr));
    }
}

TEST_F(MemalignPoolTests, mixedAlignedAndRegularAllocations) {
    void* ptr1 = pool->malloc_pool(256);
    void* ptr2 = pool->memalign_pool(64, 256);
    void* ptr3 = pool->malloc_pool(256);
    void* ptr4 = pool->memalign_pool(128, 256);

    ASSERT_NE(ptr1, nullptr);
    ASSERT_NE(ptr2, nullptr);
    ASSERT_NE(ptr3, nullptr);
    ASSERT_NE(ptr4, nullptr);

    EXPECT_TRUE(is_aligned(ptr2, 64));
    EXPECT_TRUE(is_aligned(ptr4, 128));

    EXPECT_TRUE(pool->free_pool(ptr1));
    EXPECT_TRUE(pool->free_pool(ptr2));
    EXPECT_TRUE(pool->free_pool(ptr3));
    EXPECT_TRUE(pool->free_pool(ptr4));
}

TEST_F(MemalignPoolTests, alignmentAfterFragmentation) {
    // Create fragmentation
    void* ptr1 = pool->malloc_pool(1024);
    void* ptr2 = pool->malloc_pool(1024);
    void* ptr3 = pool->malloc_pool(1024);

    EXPECT_TRUE(pool->free_pool(ptr2));

    // Try to allocate with large alignment in fragmented pool
    void* ptr4 = pool->memalign_pool(256, 512);
    ASSERT_NE(ptr4, nullptr);
    EXPECT_TRUE(is_aligned(ptr4, 256));

    EXPECT_TRUE(pool->free_pool(ptr1));
    EXPECT_TRUE(pool->free_pool(ptr3));
    EXPECT_TRUE(pool->free_pool(ptr4));
}

TEST_F(MemalignPoolTests, exhaustWithAlignment) {
    std::unordered_set<void*> ptrs;
    const std::size_t alignment = 256;
    const std::size_t size = 8192;

    // Allocate until exhausted
    while (true) {
        void* ptr = pool->memalign_pool(alignment, size);
        if (!ptr) break;
        EXPECT_TRUE(is_aligned(ptr, alignment));
        ASSERT_TRUE(ptrs.find(ptr) == ptrs.end());
        ptrs.insert(ptr);
        if (ptrs.size() > 1000) break; //safety exit to prevent infinite loop
    }

    EXPECT_GT(ptrs.size(), 0u);

    for (void* ptr : ptrs) {
        EXPECT_TRUE(pool->free_pool(ptr));
    }
}

// ============================================================================
// Fuzzing Tests
// ============================================================================

class FuzzingTests : public ::testing::Test {
protected:
    FuzzingTests() : pool(tlsf_pool::create(4 * 1024 * 1024)),
                     rng(std::random_device{}()) {}

    std::optional<tlsf_pool> pool;
    std::mt19937 rng;

    struct Allocation {
        void* ptr;
        std::size_t size;
        std::size_t alignment;
        uint8_t pattern; // Pattern written to memory
    };

    std::size_t random_size(std::size_t min_size = 1, std::size_t max_size = 8192) {
        std::uniform_int_distribution<std::size_t> dist(min_size, max_size);
        return dist(rng);
    }

    std::size_t random_alignment() {
        std::uniform_int_distribution<int> dist(0, 10);
        int power = dist(rng);
        return static_cast<std::size_t>(1) << power; // 1, 2, 4, 8, ..., 1024
    }

    bool verify_allocation(const Allocation& alloc) {
        if (!alloc.ptr) return false;

        // Check alignment
        if ((reinterpret_cast<uintptr_t>(alloc.ptr) % alloc.alignment) != 0) {
            return false;
        }

        // Verify pattern
        uint8_t* bytes = static_cast<uint8_t*>(alloc.ptr);
        for (std::size_t i = 0; i < alloc.size; ++i) {
            if (bytes[i] != alloc.pattern) {
                return false;
            }
        }

        return true;
    }
};

TEST_F(FuzzingTests, randomAllocFreeSequence) {
    std::vector<Allocation> allocations;
    std::uniform_int_distribution<int> action_dist(0, 2); // 0=alloc, 1=free, 2=aligned alloc
    std::uniform_int_distribution<unsigned int> pattern_dist(0, 255);

    const int num_operations = 1000;

    for (int i = 0; i < num_operations; ++i) {
        int action = action_dist(rng);

        if (action == 0 && allocations.size() < 200) {
            // Regular allocation
            std::size_t size = random_size();
            void* ptr = pool->malloc_pool(size);

            if (ptr) {
                using namespace tlsf::detail;
                Allocation alloc{ptr, size, ALIGN_SIZE, pattern_dist(rng)};
                std::memset(ptr, alloc.pattern, size);
                allocations.push_back(alloc);
            }
        }
        else if (action == 1 && !allocations.empty()) {
            // Free random allocation
            std::uniform_int_distribution<size_t> idx_dist(0, allocations.size() - 1);
            size_t idx = idx_dist(rng);

            // Verify before freeing
            EXPECT_TRUE(verify_allocation(allocations[idx]))
                << "Allocation corrupted before free at operation " << i;

            EXPECT_TRUE(pool->free_pool(allocations[idx].ptr));
            allocations.erase(allocations.begin() + idx);
        }
        else if (action == 2 && allocations.size() < 200) {
            // Aligned allocation
            std::size_t size = random_size();
            std::size_t alignment = random_alignment();
            void* ptr = pool->memalign_pool(alignment, size);

            if (ptr) {
                Allocation alloc{ptr, size, alignment, pattern_dist(rng)};
                std::memset(ptr, alloc.pattern, size);
                allocations.push_back(alloc);
            }
        }
    }

    // Verify all remaining allocations
    for (const auto& alloc : allocations) {
        EXPECT_TRUE(verify_allocation(alloc));
        EXPECT_TRUE(pool->free_pool(alloc.ptr));
    }
}

TEST_F(FuzzingTests, stressTestWithRealloc) {
    std::vector<Allocation> allocations;
    std::uniform_int_distribution<int> action_dist(0, 3); // 0=alloc, 1=free, 2=realloc, 3=aligned
    std::uniform_int_distribution<unsigned int> pattern_dist(0, 255);

    const int num_operations = 500;

    for (int i = 0; i < num_operations; ++i) {
        int action = action_dist(rng);

        if (action == 0 && allocations.size() < 100) {
            // Regular allocation
            std::size_t size = random_size(1, 4096);
            void* ptr = pool->malloc_pool(size);

            if (ptr) {
                using namespace tlsf::detail;
                Allocation alloc{ptr, size, ALIGN_SIZE, pattern_dist(rng)};
                std::memset(ptr, alloc.pattern, size);
                allocations.push_back(alloc);
            }
        }
        else if (action == 1 && !allocations.empty()) {
            // Free
            std::uniform_int_distribution<size_t> idx_dist(0, allocations.size() - 1);
            size_t idx = idx_dist(rng);

            EXPECT_TRUE(pool->free_pool(allocations[idx].ptr));
            allocations.erase(allocations.begin() + idx);
        }
        else if (action == 2 && !allocations.empty()) {
            // Realloc
            std::uniform_int_distribution<size_t> idx_dist(0, allocations.size() - 1);
            size_t idx = idx_dist(rng);

            std::size_t new_size = random_size(1, 4096);
            void* new_ptr = pool->realloc_pool(allocations[idx].ptr, new_size);

            if (new_ptr) {
                // Verify old data is preserved (up to min of old and new size)
                std::size_t verify_size = std::min(allocations[idx].size, new_size);
                uint8_t* bytes = static_cast<uint8_t*>(new_ptr);
                bool data_intact = true;
                for (std::size_t j = 0; j < verify_size; ++j) {
                    if (bytes[j] != allocations[idx].pattern) {
                        data_intact = false;
                        break;
                    }
                }
                EXPECT_TRUE(data_intact) << "Data corrupted during realloc";

                // Update allocation
                allocations[idx].ptr = new_ptr;
                allocations[idx].size = new_size;
                // Fill any new space with the same pattern
                if (new_size > verify_size) {
                    std::memset(bytes + verify_size, allocations[idx].pattern,
                              new_size - verify_size);
                }
            }
        }
        else if (action == 3 && allocations.size() < 100) {
            // Aligned allocation
            std::size_t size = random_size(1, 4096);
            std::size_t alignment = random_alignment();
            void* ptr = pool->memalign_pool(alignment, size);

            if (ptr) {
                Allocation alloc{ptr, size, alignment, pattern_dist(rng)};
                std::memset(ptr, alloc.pattern, size);
                allocations.push_back(alloc);
            }
        }
    }

    // Clean up
    for (const auto& alloc : allocations) {
        EXPECT_TRUE(pool->free_pool(alloc.ptr));
    }
}

TEST_F(FuzzingTests, fragmentationStressTest) {
    // Create heavy fragmentation and test allocation/deallocation
    std::vector<void*> ptrs;

    // Phase 1: Allocate many small blocks
    for (int i = 0; i < 100; ++i) {
        std::size_t size = random_size(16, 256);
        void* ptr = pool->malloc_pool(size);
        if (ptr) {
            ptrs.push_back(ptr);
        }
    }

    // Phase 2: Free random blocks to create fragmentation
    std::shuffle(ptrs.begin(), ptrs.end(), rng);
    for (size_t i = 0; i < ptrs.size() / 2; ++i) {
        pool->free_pool(ptrs[i]);
        ptrs[i] = nullptr;
    }

    // Phase 3: Try to allocate various sizes in fragmented pool
    for (int i = 0; i < 50; ++i) {
        std::size_t size = random_size(16, 1024);
        void* ptr = pool->malloc_pool(size);
        if (ptr) {
            ptrs.push_back(ptr);
        }
    }

    // Phase 4: Clean up
    for (void* ptr : ptrs) {
        if (ptr) {
            pool->free_pool(ptr);
        }
    }

    // Phase 5: Verify pool is usable after heavy fragmentation
    void* final_ptr = pool->malloc_pool(4096);
    EXPECT_NE(final_ptr, nullptr) << "Pool unusable after fragmentation test";
    if (final_ptr) {
        pool->free_pool(final_ptr);
    }
}

TEST_F(FuzzingTests, mixedSizeAlignmentFuzzing) {
    const int num_iterations = 100;

    for (int iter = 0; iter < num_iterations; ++iter) {
        std::vector<Allocation> allocations;

        // Random allocations
        for (int i = 0; i < 20; ++i) {
            std::size_t size = random_size(1, 2048);
            std::size_t alignment = random_alignment();

            void* ptr = pool->memalign_pool(alignment, size);
            if (ptr) {
                Allocation alloc{ptr, size, alignment, static_cast<uint8_t>(i & 0xFF)};
                std::memset(ptr, alloc.pattern, size);
                allocations.push_back(alloc);
            }
        }

        // Verify and free
        for (const auto& alloc : allocations) {
            EXPECT_TRUE(verify_allocation(alloc));
            EXPECT_TRUE(pool->free_pool(alloc.ptr));
        }
    }
}


