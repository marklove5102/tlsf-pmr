#include <gtest/gtest.h>
#include <random>
#include <algorithm>
#include <limits>
#include "block.hpp"

using namespace tlsf::detail;


//generic implementation
static inline int _fls_generic(unsigned int word){
    int bit = 32;

    if (!word) bit -= 1;
    if (!(word & 0xffff0000)) { word <<= 16; bit -= 16;}
    if (!(word & 0xff000000)) { word <<= 8; bit -= 8;}
    if (!(word & 0xf0000000)) { word <<= 4; bit -= 4;}
    if (!(word & 0xc0000000)) { word <<= 2; bit -= 2;}
    if (!(word & 0x80000000)) { word <<= 1; bit -= 1;}

    return bit;
}

int test_ffs(unsigned int word){
    return _fls_generic(word & (~word +1)) -1;
}

int test_fls(unsigned int word){
    return _fls_generic(word)-1;
}


TEST(UtilityTests, bitsetArithmeticCorrect){
    EXPECT_EQ(tlsf_ffs(0), -1);
    EXPECT_EQ(tlsf_ffs(1), 0);
    EXPECT_EQ(tlsf_ffs(0x80000000), 31);
    EXPECT_EQ(tlsf_ffs(0x80008000), 15);

    EXPECT_EQ(tlsf_fls(0), -1);
    EXPECT_EQ(tlsf_fls(1), 0);
    EXPECT_EQ(tlsf_fls(0x80000008), 31);
    EXPECT_EQ(tlsf_fls(0x7FFFFFFF), 30);

#ifdef TLSF_64BIT
    EXPECT_EQ(tlsf_fls_sizet(0x80000000), 31);
    EXPECT_EQ(tlsf_fls_sizet(0x100000000), 32);
    EXPECT_EQ(tlsf_fls_sizet(0xffffffffffffffff), 63);
#endif

}

TEST(UtilityTests, alignmentCorrect){
    EXPECT_EQ(align_up(998, 8), 1000);
    EXPECT_EQ(align_down(998, 8), 992);
    EXPECT_EQ(align_up(500, 32), 512);
    EXPECT_EQ(align_down(500, 32), 480);
}

TEST(UtilityTests, bitmapMapping){
    int fli, sli;
    int size = 1000;
    //minimum block size is 256 bytes. 
    //a size request of 1000 will return a 1008 size block.
    //first level index is 2: 512-1024 bytes
    //second level index is 31: 512/32 * 31 = 496 bytes
    //512 + 496 = 1008
    mapping_search(size, &fli, &sli);
    EXPECT_EQ(fli, 2);
    EXPECT_EQ(sli, 31);

    size = 1500;
    //first level index is 3: 1024-2048
    //second level index is 15: 1024/32* 15 = 480 bytes
    //1024+480 = 1504 bytes
    mapping_search(size, &fli, &sli);
    EXPECT_EQ(fli, 3);
    EXPECT_EQ(sli, 15);
}

TEST(UtilityTests, ffsFuzzedResults) {
    std::mt19937 rng;
    std::uniform_int_distribution<unsigned int> dist(std::numeric_limits<unsigned int>::min(), std::numeric_limits<unsigned int>::max());
    const int num_comparisons = 1000;
    for (int i = 0; i < num_comparisons; i++) {
        unsigned int val = dist(rng);
        EXPECT_EQ(tlsf_ffs(val), test_ffs(val));
    }
}

TEST(UtilityTests, flsFuzzedResults) {
    std::mt19937 rng;
    std::uniform_int_distribution<unsigned int> dist(std::numeric_limits<unsigned int>::min(), std::numeric_limits<unsigned int>::max());
    const int num_comparisons = 1000;
    for (int i = 0; i < num_comparisons; i++) {
        unsigned int val = dist(rng);
        EXPECT_EQ(tlsf_fls(val), test_fls(val));
    }
}