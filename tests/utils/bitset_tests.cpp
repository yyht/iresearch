//
// IResearch search engine 
// 
// Copyright (c) 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#include "tests_shared.hpp"
#include "utils/bitset.hpp"

using namespace iresearch;

TEST(bitset_tests, static_functions) {
  // bit index for the specified number
  ASSERT_EQ(7, bitset::bit(7));
  ASSERT_EQ(65 % (sizeof(bitset::word_t)*8), bitset::bit(65));

  // bit offset for the specified word
  ASSERT_EQ(0, bitset::bit_offset(0));
  ASSERT_EQ(2*(sizeof(bitset::word_t)*8), bitset::bit_offset(2));
}

TEST(bitset_tests, ctor) {
  // zero size bitset
  {
    const bitset::index_t words = 0;
    const bitset::index_t size = 0;
    const bitset bs(size);
    ASSERT_EQ(nullptr, bs.data());
    ASSERT_EQ(size, bs.size());
    ASSERT_EQ(words, bs.words());
    ASSERT_EQ(0, bs.capacity());
    ASSERT_EQ(0, bs.count());
    ASSERT_TRUE(bs.none());
    ASSERT_FALSE(bs.any());
    ASSERT_TRUE(bs.all());
  }

  // less that 1 word size bitset
  {
    const bitset::index_t words = 1;
    const bitset::index_t size = 32;
    const bitset bs( size );
    ASSERT_NE(nullptr, bs.data());
    ASSERT_EQ(size, bs.size());
    ASSERT_EQ(words, bs.words());
    ASSERT_EQ(sizeof(bitset::word_t)*8*words, bs.capacity());
    ASSERT_EQ(0, bs.count());
    ASSERT_TRUE(bs.none());
    ASSERT_FALSE(bs.any());
    ASSERT_FALSE(bs.all());
  }

  // uint64_t size bitset
  {
    const bitset::index_t size = 64;
    const bitset::index_t words = 1;
    const bitset bs(size);
    ASSERT_NE(nullptr, bs.data());
    ASSERT_EQ(size, bs.size());
    ASSERT_EQ(words, bs.words());
    ASSERT_EQ(size, bs.capacity());
    ASSERT_EQ(0, bs.count());
    ASSERT_TRUE(bs.none());
    ASSERT_FALSE(bs.any());
    ASSERT_FALSE(bs.all());
  }

  // nonzero size bitset
  {
    const bitset::index_t words = 2;
    const bitset::index_t size = 78;
    const bitset bs( size );
    ASSERT_NE(nullptr, bs.data());
    ASSERT_EQ(size, bs.size());
    ASSERT_EQ(words, bs.words());
    ASSERT_EQ(sizeof(bitset::word_t)*8*words, bs.capacity());
    ASSERT_EQ(0, bs.count());
    ASSERT_TRUE(bs.none());
    ASSERT_FALSE(bs.any());
    ASSERT_FALSE(bs.all());
  }
}

TEST(bitset_tests, set_unset) {
  const bitset::index_t words = 3;
  const bitset::index_t size = 155;
  bitset bs(size);
  ASSERT_NE(nullptr, bs.data());
  ASSERT_EQ(size, bs.size());
  ASSERT_EQ(words, bs.words());
  ASSERT_EQ(sizeof(bitset::word_t)*8*words, bs.capacity());
  ASSERT_EQ(0, bs.count());
  ASSERT_TRUE(bs.none());
  ASSERT_FALSE(bs.any());
  ASSERT_FALSE(bs.all());

  // set
  const bitset::index_t i = 43;
  ASSERT_FALSE(bs.test(i));
  bs.set(i);
  ASSERT_TRUE(bs.test(i));
  ASSERT_EQ(1, bs.count());

  // unset
  bs.unset(i);
  ASSERT_FALSE(bs.test(i));
  ASSERT_EQ(0, bs.count());

  // reset
  bs.reset(i, true);
  ASSERT_TRUE(bs.test(i));
  bs.reset(i, false);
  ASSERT_FALSE(bs.test(i));
}

TEST(bitset_tests, reset) {
  bitset bs;
  ASSERT_EQ(nullptr, bs.data());
  ASSERT_EQ(0, bs.size());
  ASSERT_EQ(0, bs.words());
  ASSERT_EQ(0, bs.capacity());
  ASSERT_EQ(0, bs.count());
  ASSERT_TRUE(bs.none());
  ASSERT_FALSE(bs.any());
  ASSERT_TRUE(bs.all());

  // reset to bigger size
  bitset::index_t words = 3;
  bitset::index_t size = 155;

  bs.reset(size);
  ASSERT_NE(nullptr, bs.data());
  ASSERT_EQ(size, bs.size());
  ASSERT_EQ(words, bs.words());
  ASSERT_EQ(sizeof(bitset::word_t)*8*words, bs.capacity());
  ASSERT_EQ(0, bs.count());
  ASSERT_TRUE(bs.none());
  ASSERT_FALSE(bs.any());
  ASSERT_FALSE(bs.all());
  bs.set(42);
  ASSERT(1, bs.count());
  bs.set(42);
  ASSERT(1, bs.count());
  bs.set(73);
  ASSERT(2, bs.count());
  ASSERT_FALSE(bs.none() );
  ASSERT_TRUE(bs.any());
  ASSERT_FALSE(bs.all());
  const auto* prev_data = bs.data();

  // reset to smaller size
  words = 2;
  size = 89;

  bs.reset(size); // reset to smaller size
  ASSERT_NE(nullptr, bs.data());
  ASSERT_EQ(size, bs.size());
  ASSERT_EQ(words, bs.words());
  ASSERT_EQ(prev_data, bs.data()); // storage haven't changed
  ASSERT_EQ(sizeof(bitset::word_t)*8*words, bs.capacity());
  ASSERT_EQ(0, bs.count());
  ASSERT_TRUE(bs.none()); // content cleared
  ASSERT_FALSE(bs.any()); // content cleared
  ASSERT_FALSE(bs.all()); // content cleared
  bs.set(42);
  ASSERT(1, bs.count());
  bs.set(42);
  ASSERT(1, bs.count());
  bs.set(73); 
  ASSERT(2, bs.count());
  ASSERT_FALSE(bs.none() );
  ASSERT_TRUE(bs.any());
  ASSERT_FALSE(bs.all());

  // reset to bigger size
  words = 5;
  size = 319;

  bs.reset(size); // reset to smaller size
  ASSERT_NE(nullptr, bs.data());
  ASSERT_EQ(size, bs.size());
  ASSERT_EQ(words, bs.words());
  ASSERT_NE(prev_data, bs.data()); // storage was reallocated
  ASSERT_EQ(sizeof(bitset::word_t)*8*words, bs.capacity());
  ASSERT_EQ(0, bs.count());
  ASSERT_TRUE(bs.none()); // content cleared
  ASSERT_FALSE(bs.any()); // content cleared
  ASSERT_FALSE(bs.all()); // content cleared
  bs.set(42);
  ASSERT(1, bs.count());
  bs.set(42);
  ASSERT(1, bs.count());
  bs.set(73);
  ASSERT(2, bs.count());
  ASSERT_FALSE(bs.none());
  ASSERT_TRUE(bs.any());
  ASSERT_FALSE(bs.all());
}

TEST(bitset_tests, clear_count) {
  const bitset::index_t words = 3;
  const bitset::index_t size = 155;
  bitset bs(size);
  ASSERT_NE(nullptr, bs.data());
  ASSERT_EQ(size, bs.size());
  ASSERT_EQ(words, bs.words());
  ASSERT_EQ(sizeof(bitset::word_t)*8*words, bs.capacity());
  ASSERT_EQ(0, bs.count());
  ASSERT_TRUE(bs.none());
  ASSERT_FALSE(bs.any());
  ASSERT_FALSE(bs.all());

  bs.set(42);
  ASSERT(1, bs.count());
  bs.set(42);
  ASSERT(1, bs.count());
  bs.set(73);
  ASSERT(2, bs.count());
  ASSERT_FALSE(bs.none());
  ASSERT_TRUE(bs.any());
  ASSERT_FALSE(bs.all());

  // set almost all bits
  const bitset::index_t end = 100;
  for (bitset::index_t i = 0; i < end; ++i) {
    bs.set(i);
  }
  ASSERT_EQ(end, bs.count());
  ASSERT_FALSE(bs.none());
  ASSERT_TRUE(bs.any());
  ASSERT_FALSE(bs.all());

  // set almost all
  for (bitset::index_t i = 0; i < bs.size(); ++i) {
    bs.set(i);
  }
  ASSERT_EQ(bs.size(), bs.count());
  ASSERT_FALSE(bs.none());
  ASSERT_TRUE(bs.any());
  ASSERT_TRUE(bs.all());

  // clear all bits
  bs.clear();
  ASSERT_TRUE(bs.none());
  ASSERT_FALSE(bs.any());
  ASSERT_FALSE(bs.all());
}

TEST(bitset_tests, memset) {
  // empty bitset
  {
    bitset bs;
    ASSERT_EQ(nullptr, bs.data());
    ASSERT_EQ(0, bs.size());
    ASSERT_EQ(0, bs.words());
    ASSERT_EQ(0, bs.capacity());
    ASSERT_EQ(0, bs.count());
    ASSERT_TRUE(bs.none());
    ASSERT_FALSE(bs.any());
    ASSERT_TRUE(bs.all());

    bs.memset(0x723423);

    ASSERT_EQ(0, bs.count());
    ASSERT_TRUE(bs.none());
    ASSERT_FALSE(bs.any());
    ASSERT_TRUE(bs.all());
  }

  // single word bitset
  {
    const bitset::index_t words = 1;
    const bitset::index_t size = 15;

    bitset bs(size);
    ASSERT_NE(nullptr, bs.data());
    ASSERT_EQ(size, bs.size());
    ASSERT_EQ(words, bs.words());
    ASSERT_EQ(words*sizeof(bitset::word_t)*8, bs.capacity());
    ASSERT_EQ(0, bs.count());
    ASSERT_TRUE(bs.none());
    ASSERT_FALSE(bs.any());
    ASSERT_FALSE(bs.all());

    bitset::word_t value = 0x723423;
    bs.memset(value);

    ASSERT_EQ(6, bs.count()); // only first 15 bits from 'value' are set
    ASSERT_EQ(bs.word(0), value & 0x7FFF);
    ASSERT_FALSE(bs.none());
    ASSERT_TRUE(bs.any());
    ASSERT_FALSE(bs.all());

    value = 0xFFFFFFFF;
    bs.memset(value); // set another value

    ASSERT_EQ(size, bs.count()); // only first 15 bits from 'value' are set
    ASSERT_EQ(bs.word(0), value& 0x7FFF);
    ASSERT_FALSE(bs.none());
    ASSERT_TRUE(bs.any());
    ASSERT_TRUE(bs.all());
  }

  // multiple words bitset
  {
    const bitset::index_t words = 2;
    const bitset::index_t size = 78;

    bitset bs(size);
    ASSERT_NE(nullptr, bs.data());
    ASSERT_EQ(size, bs.size());
    ASSERT_EQ(words, bs.words());
    ASSERT_EQ(words*sizeof(bitset::word_t)*8, bs.capacity());
    ASSERT_EQ(0, bs.count());
    ASSERT_TRUE(bs.none());
    ASSERT_FALSE(bs.any());
    ASSERT_FALSE(bs.all());

    const uint64_t value = UINT64_C(0x14FFFFFFFFFFFFFF);
    bs.memset(value);

    ASSERT_EQ(58, bs.count()); // only first 15 bits from 'value' are set
    ASSERT_EQ(bs.word(0), value); // 1st word
    ASSERT_EQ(bs.word(64), 0); // 2nd word
    ASSERT_FALSE(bs.none());
    ASSERT_TRUE(bs.any());
    ASSERT_FALSE(bs.all());
  }
}
