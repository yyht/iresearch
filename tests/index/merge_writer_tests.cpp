//
// IResearch search engine 
// 
// Copyright (c) 2016 by EMC Corporation, All Rights Reserved // 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#include "tests_shared.hpp"
#include "index_tests.hpp"
#include "formats/formats_10.hpp"
#include "iql/query_builder.hpp"
#include "store/memory_directory.hpp"
#include "utils/type_limits.hpp"
#include "index/merge_writer.hpp"

namespace tests {
  class merge_writer_tests: public ::testing::Test {

    virtual void SetUp() {
      // Code here will be called immediately after the constructor (right before each test).
    }

    virtual void TearDown() {
      // Code here will be called immediately after each test (right before the destructor).
    }
  };

  template<typename T>
  void validate_terms(
    const iresearch::term_reader& terms,
    uint64_t doc_count,
    const iresearch::bytes_ref& min,
    const iresearch::bytes_ref& max,
    size_t term_size,
    const iresearch::flags& term_features,
    std::unordered_map<T, std::unordered_set<iresearch::doc_id_t>>& expected_terms,
    size_t* frequency = nullptr,
    std::vector<uint32_t>* position = nullptr
  ) {
    ASSERT_EQ(doc_count, terms.docs_count());
    ASSERT_EQ((max), (terms.max)());
    ASSERT_EQ((min), (terms.min)());
    ASSERT_EQ(term_size, terms.size());
    ASSERT_EQ(term_features, terms.meta().features);

    for (auto term_itr = terms.iterator(); term_itr->next();) {
      auto itr = expected_terms.find(term_itr->value());

      ASSERT_NE(expected_terms.end(), itr);

      for (auto docs_itr = term_itr->postings(term_features); docs_itr->next();) {
        auto& attrs = docs_itr->attributes();

        ASSERT_EQ(1, itr->second.erase(docs_itr->value()));
        ASSERT_EQ(1 + (frequency ? 1 : 0) + (position ? 1 : 0), attrs.size());
        ASSERT_TRUE(attrs.contains(iresearch::document::type()));

        if (frequency) {
          ASSERT_TRUE(attrs.contains(iresearch::frequency::type()));
          ASSERT_EQ(*frequency, attrs.get<iresearch::frequency>()->value);
        }

        if (position) {
          ASSERT_TRUE(attrs.contains(iresearch::position::type()));

          for (auto pos: *position) {
            ASSERT_TRUE(attrs.get<iresearch::position>()->next());
            ASSERT_EQ(pos, attrs.get<iresearch::position>()->value());
          }

          ASSERT_FALSE(attrs.get<iresearch::position>()->next());
        }
      }

      ASSERT_TRUE(itr->second.empty());
      expected_terms.erase(itr);
    }

    ASSERT_TRUE(expected_terms.empty());
  }
}

using namespace tests;

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

TEST_F(merge_writer_tests, test_merge_writer_columns_remove) {
  iresearch::flags STRING_FIELD_FEATURES{ iresearch::frequency::type(), iresearch::position::type() };
  iresearch::flags TEXT_FIELD_FEATURES{ iresearch::frequency::type(), iresearch::position::type(), iresearch::offset::type(), iresearch::payload::type() };

  std::string string1;
  std::string string2;
  std::string string3;
  std::string string4;

  string1.append("string1_data");
  string2.append("string2_data");
  string3.append("string3_data");
  string4.append("string4_data");

  tests::document doc1; // doc_int, doc_string
  tests::document doc2; // doc_string, doc_int
  tests::document doc3; // doc_string, doc_int
  tests::document doc4; // doc_string, another_column

  doc1.insert(std::make_shared<tests::int_field>()); {
    auto& field = doc1.indexed.back<tests::int_field>();
    field.name(iresearch::string_ref("doc_int"));
    field.value(42 * 1);
  }
  doc1.insert(
    std::make_shared<tests::templates::string_field>("doc_string", string1)
  );

  doc2.insert(std::make_shared<tests::templates::string_field>("doc_string", string2));
  doc2.insert(std::make_shared<tests::int_field>()); 
  {
    auto& field = doc2.indexed.back<tests::int_field>();
    field.name(iresearch::string_ref("doc_int"));
    field.value(42 * 2);
  }
  
  doc3.insert(std::make_shared<tests::templates::string_field>("doc_string", string3));
  doc3.insert(std::make_shared<tests::int_field>()); {
    auto& field = doc3.indexed.back<tests::int_field>();
    field.name(iresearch::string_ref("doc_int"));
    field.value(42 * 3);
  }
  
  doc4.insert(std::make_shared<tests::templates::string_field>("doc_string", string4));
  doc4.insert(std::make_shared<tests::templates::string_field>("another_column", "another_value"));
  
  iresearch::version10::format codec;
  iresearch::format::ptr codec_ptr(&codec, [](iresearch::format*)->void{});
  iresearch::memory_directory dir;

  // populate directory
  {
    auto query_doc4 = iresearch::iql::query_builder().build("doc_string==string4_data", std::locale::classic());
    auto writer = iresearch::index_writer::make(dir, codec_ptr, iresearch::OM_CREATE);
    ASSERT_TRUE(writer->insert(doc1.indexed.end(), doc1.indexed.end(), doc1.stored.begin(), doc1.stored.end()));
    ASSERT_TRUE(writer->insert(doc3.indexed.end(), doc3.indexed.end(), doc3.stored.begin(), doc3.stored.end()));
    writer->commit();
    ASSERT_TRUE(writer->insert(doc2.indexed.end(), doc2.indexed.end(), doc2.stored.begin(), doc2.stored.end()));
    ASSERT_TRUE(writer->insert(doc4.indexed.begin(), doc4.indexed.end(), doc4.stored.begin(), doc4.stored.end()));
    writer->commit();
    writer->remove(std::move(query_doc4.filter));
    writer->commit();
    writer->close();
  }
  
  auto reader = iresearch::directory_reader::open(dir, codec_ptr);
  iresearch::merge_writer writer(dir, codec_ptr, "merged");

  ASSERT_EQ(2, reader.size());
  ASSERT_EQ(2, reader[0].docs_count());
  ASSERT_EQ(2, reader[1].docs_count());

  // check for columns segment 0
  {
    auto& segment = reader[0];

    auto columns = segment.columns();
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_int", columns->value().name);
    ASSERT_EQ(0, columns->value().id);
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_string", columns->value().name);
    ASSERT_EQ(1, columns->value().id);
    ASSERT_FALSE(columns->next());

    // check 'doc_int' column
    {
      std::unordered_map<int, iresearch::doc_id_t> expected_values{
        { 1 * 42, 1 },
        { 3 * 42, 2 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_zvint(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_int'
      auto* meta = segment.column("doc_int");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }
    
    // check 'doc_string' column
    {
      std::unordered_map <std::string, iresearch::doc_id_t > expected_values{
        { "string1_data", 1 },
        { "string3_data", 2 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_string<std::string>(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_string'
      auto* meta = segment.column("doc_string");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }

    // check wrong column
    {
      size_t calls_count = 0;
      auto reader = [&calls_count] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        return true;
      };

      ASSERT_EQ(nullptr, segment.column("invalid_column"));
      ASSERT_FALSE(
        segment.visit(iresearch::type_limits<iresearch::type_t::field_id_t>::invalid(), reader)
      );
      ASSERT_EQ(0, calls_count);
    }
  }
  
  // check for columns segment 1
  {
    auto& segment = reader[1];

    auto columns = segment.columns();
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("another_column", columns->value().name);
    ASSERT_EQ(2, columns->value().id);
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_int", columns->value().name);
    ASSERT_EQ(1, columns->value().id);
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_string", columns->value().name);
    ASSERT_EQ(0, columns->value().id);
    ASSERT_FALSE(columns->next());

    // check 'doc_int' column
    {
      std::unordered_map<int, iresearch::doc_id_t> expected_values{
        { 2 * 42, 1 },
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_zvint(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_int'
      auto* meta = segment.column("doc_int");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }
    
    // check 'doc_string' column
    {
      std::unordered_map <std::string, iresearch::doc_id_t > expected_values{
        { "string2_data", 1 },
        { "string4_data", 2 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_string<std::string>(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_string'
      auto* meta = segment.column("doc_string");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }
    
    // check 'another_column' column
    {
      std::unordered_map <std::string, iresearch::doc_id_t > expected_values{
        { "another_value", 2 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_string<std::string>(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'another_column'
      auto* meta = segment.column("another_column");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }

    // check invalid column 
    {
      size_t calls_count = 0;
      auto reader = [&calls_count] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        return true;
      };

      ASSERT_EQ(nullptr, segment.column("invalid_column"));
      ASSERT_FALSE(
        segment.visit(iresearch::type_limits<iresearch::type_t::field_id_t>::invalid(), reader)
      );
      ASSERT_EQ(0, calls_count);
    }
  }
  
  writer.add(reader[0]);
  writer.add(reader[1]);

  std::string filename;
  iresearch::segment_meta meta;

  writer.flush(filename, meta);
  {
    auto segment = iresearch::segment_reader::open(dir, meta);
    ASSERT_EQ(3, segment.docs_count());

    auto columns = segment.columns();
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_int", columns->value().name);
    ASSERT_EQ(0, columns->value().id); // 0 since 'doc_int' < 'doc_string'
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_string", columns->value().name);
    ASSERT_EQ(1, columns->value().id);
    ASSERT_FALSE(columns->next());
    
    // check 'doc_int' column
    {
      std::unordered_map<int, iresearch::doc_id_t> expected_values{
        // segment 0
        { 1 * 42, 1 },
        { 3 * 42, 2 },
        // segment 1
        { 2 * 42, 3 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_zvint(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_int'
      auto* meta = segment.column("doc_int");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }

    // check 'doc_string' column
    {
      std::unordered_map <std::string, iresearch::doc_id_t > expected_values{
        // segment 0
        { "string1_data", 1 },
        { "string3_data", 2 },
        // segment 1
        { "string2_data", 3 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_string<std::string>(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_string'
      auto* meta = segment.column("doc_string");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }

    // check that 'another_column' has been removed
    {
      size_t calls_count = 0;
      auto reader = [&calls_count] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        return true;
      };

      ASSERT_EQ(nullptr, segment.column("another_column"));
      ASSERT_FALSE(segment.visit(2, reader));
      ASSERT_EQ(0, calls_count);
    }
  }
}

TEST_F(merge_writer_tests, test_merge_writer_columns) {
  iresearch::flags STRING_FIELD_FEATURES{ iresearch::frequency::type(), iresearch::position::type() };
  iresearch::flags TEXT_FIELD_FEATURES{ iresearch::frequency::type(), iresearch::position::type(), iresearch::offset::type(), iresearch::payload::type() };

  std::string string1;
  std::string string2;
  std::string string3;
  std::string string4;

  string1.append("string1_data");
  string2.append("string2_data");
  string3.append("string3_data");
  string4.append("string4_data");

  tests::document doc1; // doc_string, doc_int
  tests::document doc2; // doc_string, doc_int
  tests::document doc3; // doc_string, doc_int
  tests::document doc4; // doc_string

  doc1.insert(std::make_shared<tests::int_field>()); 
  {
    auto& field = doc1.indexed.back<tests::int_field>();
    field.name(iresearch::string_ref("doc_int"));
    field.value(42 * 1);
  }
  doc1.insert(std::make_shared<tests::templates::string_field>("doc_string", string1));

  doc2.insert(std::make_shared<tests::templates::string_field>("doc_string", string2));
  doc2.insert(std::make_shared<tests::int_field>()); {
    auto& field = doc2.indexed.back<tests::int_field>();
    field.name(iresearch::string_ref("doc_int"));
    field.value(42 * 2);
  }
  
  doc3.insert(std::make_shared<tests::templates::string_field>("doc_string", string3));
  doc3.insert(std::make_shared<tests::int_field>()); 
  {
    auto& field = doc3.indexed.back<tests::int_field>();
    field.name(iresearch::string_ref("doc_int"));
    field.value(42 * 3);
  }

  doc4.insert(std::make_shared<tests::templates::string_field>("doc_string", string4));

  iresearch::version10::format codec;
  iresearch::format::ptr codec_ptr(&codec, [](iresearch::format*)->void{});
  iresearch::memory_directory dir;

  // populate directory
  {
    auto writer = iresearch::index_writer::make(dir, codec_ptr, iresearch::OM_CREATE);
    ASSERT_TRUE(writer->insert(doc1.indexed.end(), doc1.indexed.end(), doc1.stored.begin(), doc1.stored.end()));
    ASSERT_TRUE(writer->insert(doc3.indexed.end(), doc3.indexed.end(), doc3.stored.begin(), doc3.stored.end()));
    writer->commit();
    ASSERT_TRUE(writer->insert(doc2.indexed.end(), doc2.indexed.end(), doc2.stored.begin(), doc2.stored.end()));
    ASSERT_TRUE(writer->insert(doc4.indexed.end(), doc4.indexed.end(), doc4.stored.begin(), doc4.stored.end()));
    writer->commit();
    writer->close();
  }

  auto reader = iresearch::directory_reader::open(dir, codec_ptr);
  iresearch::merge_writer writer(dir, codec_ptr, "merged");

  ASSERT_EQ(2, reader.size());
  ASSERT_EQ(2, reader[0].docs_count());
  ASSERT_EQ(2, reader[1].docs_count());

  // check for columns segment 0
  {
    auto& segment = reader[0];

    auto columns = segment.columns();
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_int", columns->value().name);
    ASSERT_EQ(0, columns->value().id);
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_string", columns->value().name);
    ASSERT_EQ(1, columns->value().id);
    ASSERT_FALSE(columns->next());
    ASSERT_FALSE(columns->next());

    // check 'doc_int' column
    {
      std::unordered_map<int, iresearch::doc_id_t> expected_values{
        { 1 * 42, 1 },
        { 3 * 42, 2 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_zvint(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_int'
      auto* meta = segment.column("doc_int");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }
    
    // check 'doc_string' column
    {
      std::unordered_map <std::string, iresearch::doc_id_t > expected_values{
        { "string1_data", 1 },
        { "string3_data", 2 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_string<std::string>(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_string'
      auto* meta = segment.column("doc_string");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }

    // check wrong column
    {
      size_t calls_count = 0;
      auto reader = [&calls_count] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        return true;
      };

      ASSERT_EQ(nullptr, segment.column("invalid_column"));
      ASSERT_FALSE(
        segment.visit(iresearch::type_limits<iresearch::type_t::field_id_t>::invalid(), reader)
      );
      ASSERT_EQ(0, calls_count);
    }
  }

  // check for columns segment 1
  {
    auto& segment = reader[1];

    auto columns = segment.columns();
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_int", columns->value().name);
    ASSERT_EQ(1, columns->value().id);
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_string", columns->value().name);
    ASSERT_EQ(0, columns->value().id);
    ASSERT_FALSE(columns->next());

    // check 'doc_int' column
    {
      std::unordered_map<int, iresearch::doc_id_t> expected_values{
        { 2 * 42, 1 },
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_zvint(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_int'
      auto* meta = segment.column("doc_int");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }
    
    // check 'doc_string' column
    {
      std::unordered_map <std::string, iresearch::doc_id_t > expected_values{
        { "string2_data", 1 },
        { "string4_data", 2 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_string<std::string>(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_string'
      auto* meta = segment.column("doc_string");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }

    // check wrong column
    {
      size_t calls_count = 0;
      auto reader = [&calls_count] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        return true;
      };

      ASSERT_EQ(nullptr, segment.column("invalid_column"));
      ASSERT_FALSE(
        segment.visit(iresearch::type_limits<iresearch::type_t::field_id_t>::invalid(), reader)
      );
      ASSERT_EQ(0, calls_count);
    }
  }

  writer.add(reader[0]);
  writer.add(reader[1]);

  std::string filename;
  iresearch::segment_meta meta;

  writer.flush(filename, meta);
  {
    auto segment = iresearch::segment_reader::open(dir, meta);
    ASSERT_EQ(4, segment.docs_count());

    auto columns = segment.columns();
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_int", columns->value().name);
    ASSERT_EQ(0, columns->value().id); // 0 since 'doc_int' < 'doc_string'
    ASSERT_TRUE(columns->next());
    ASSERT_EQ("doc_string", columns->value().name);
    ASSERT_EQ(1, columns->value().id);
    ASSERT_FALSE(columns->next());

    // check 'doc_int' column
    {
      std::unordered_map<int, iresearch::doc_id_t> expected_values{
        // segment 0
        { 1 * 42, 1 },
        { 3 * 42, 2 },
        // segment 1
        { 2 * 42, 3 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_zvint(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_int'
      auto* meta = segment.column("doc_int");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }

    // check 'doc_string' column
    {
      std::unordered_map <std::string, iresearch::doc_id_t > expected_values{
        // segment 0
        { "string1_data", 1 },
        { "string3_data", 2 },
        // segment 1
        { "string2_data", 3 },
        { "string4_data", 4 }
      };

      size_t calls_count = 0;
      auto reader = [&calls_count, &expected_values] (iresearch::doc_id_t doc, data_input& in) {
        ++calls_count;
        const auto actual_value = iresearch::read_string<std::string>(in);

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        return true;
      };

      // read values for 'doc_string'
      auto* meta = segment.column("doc_string");
      ASSERT_NE(nullptr, meta);
      ASSERT_TRUE(segment.visit(meta->id, reader));
      ASSERT_EQ(expected_values.size(), calls_count);
    }
  }
}

TEST_F(merge_writer_tests, test_merge_writer) {
  iresearch::version10::format codec;
  iresearch::format::ptr codec_ptr(&codec, [](iresearch::format*)->void{});
  iresearch::memory_directory dir;

  iresearch::bstring bytes1;
  iresearch::bstring bytes2;
  iresearch::bstring bytes3;

  bytes1.append(iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("bytes1_data")));
  bytes2.append(iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("bytes2_data")));
  bytes3.append(iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("bytes3_data")));

  iresearch::flags STRING_FIELD_FEATURES{ iresearch::frequency::type(), iresearch::position::type() };
  iresearch::flags TEXT_FIELD_FEATURES{ iresearch::frequency::type(), iresearch::position::type(), iresearch::offset::type(), iresearch::payload::type() };

  std::string string1;
  std::string string2;
  std::string string3;
  std::string string4;

  string1.append("string1_data");
  string2.append("string2_data");
  string3.append("string3_data");
  string4.append("string4_data");

  std::string text1;
  std::string text2;
  std::string text3;

  text1.append("text1_data");
  text2.append("text2_data");
  text3.append("text3_data");

  tests::document doc1;
  tests::document doc2;
  tests::document doc3;
  tests::document doc4;

  doc1.insert(std::make_shared<tests::binary_field>()); {
    auto& field = doc1.indexed.back<tests::binary_field>();
    field.name(iresearch::string_ref("doc_bytes"));
    field.value(bytes1);
    field.features().add<iresearch::norm>();
    field.boost(1.5f);
  }
  doc2.insert(std::make_shared<tests::binary_field>()); {
    auto& field = doc2.indexed.back<tests::binary_field>();
    field.name(iresearch::string_ref("doc_bytes"));
    field.value(bytes2);
  }
  doc3.insert(std::make_shared<tests::binary_field>()); {
    auto& field = doc3.indexed.back<tests::binary_field>();
    field.name(iresearch::string_ref("doc_bytes"));
    field.value(bytes3);
    field.features().add<iresearch::norm>();
    field.boost(2.5f);
  }
  doc1.insert(std::make_shared<tests::double_field>()); {
    auto& field = doc1.indexed.back<tests::double_field>();
    field.name(iresearch::string_ref("doc_double"));
    field.value(2.718281828 * 1);
  }
  doc2.insert(std::make_shared<tests::double_field>()); {
    auto& field = doc2.indexed.back<tests::double_field>();
    field.name(iresearch::string_ref("doc_double"));
    field.value(2.718281828 * 2);
  }
  doc3.insert(std::make_shared<tests::double_field>()); {
    auto& field = doc3.indexed.back<tests::double_field>();
    field.name(iresearch::string_ref("doc_double"));
    field.value(2.718281828 * 3);
  }
  doc1.insert(std::make_shared<tests::float_field>()); {
    auto& field = doc1.indexed.back<tests::float_field>();
    field.name(iresearch::string_ref("doc_float"));
    field.value(3.1415926535f * 1);
  }
  doc2.insert(std::make_shared<tests::float_field>()); {
    auto& field = doc2.indexed.back<tests::float_field>();
    field.name(iresearch::string_ref("doc_float"));
    field.value(3.1415926535f * 2);
  }
  doc3.insert(std::make_shared<tests::float_field>()); {
    auto& field = doc3.indexed.back<tests::float_field>();
    field.name(iresearch::string_ref("doc_float"));
    field.value(3.1415926535f * 3);
  }
  doc1.insert(std::make_shared<tests::int_field>()); {
    auto& field = doc1.indexed.back<tests::int_field>();
    field.name(iresearch::string_ref("doc_int"));
    field.value(42 * 1);
  }
  doc2.insert(std::make_shared<tests::int_field>()); {
    auto& field = doc2.indexed.back<tests::int_field>();
    field.name(iresearch::string_ref("doc_int"));
    field.value(42 * 2);
  }
  doc3.insert(std::make_shared<tests::int_field>()); {
    auto& field = doc3.indexed.back<tests::int_field>();
    field.name(iresearch::string_ref("doc_int"));
    field.value(42 * 3);
  }
  doc1.insert(std::make_shared<tests::long_field>()); {
    auto& field = doc1.indexed.back<tests::long_field>();
    field.name(iresearch::string_ref("doc_long"));
    field.value(12345 * 1);
  }
  doc2.insert(std::make_shared<tests::long_field>()); {
    auto& field = doc2.indexed.back<tests::long_field>();
    field.name(iresearch::string_ref("doc_long"));
    field.value(12345 * 2);
  }
  doc3.insert(std::make_shared<tests::long_field>()); {
    auto& field = doc3.indexed.back<tests::long_field>();
    field.name(iresearch::string_ref("doc_long"));
    field.value(12345 * 3);
  }
  doc1.insert(std::make_shared<tests::templates::string_field>("doc_string", string1));
  doc2.insert(std::make_shared<tests::templates::string_field>("doc_string", string2));
  doc3.insert(std::make_shared<tests::templates::string_field>("doc_string", string3));
  doc4.insert(std::make_shared<tests::templates::string_field>("doc_string", string4));
  doc1.indexed.push_back(std::make_shared<tests::templates::text_field<iresearch::string_ref>>("doc_text", text1));
  doc2.indexed.push_back(std::make_shared<tests::templates::text_field<iresearch::string_ref>>("doc_text", text2));
  doc3.indexed.push_back(std::make_shared<tests::templates::text_field<iresearch::string_ref>>("doc_text", text3));

  // populate directory
  {
    auto query_doc4 = iresearch::iql::query_builder().build("doc_string==string4_data", std::locale::classic());
    auto writer = iresearch::index_writer::make(dir, codec_ptr, iresearch::OM_CREATE);

    ASSERT_TRUE(writer->insert(
      doc1.indexed.begin(), doc1.indexed.end(),
      doc1.stored.begin(), doc1.stored.end()
    ));
    ASSERT_TRUE(writer->insert(
      doc2.indexed.begin(), doc2.indexed.end(),
      doc2.stored.begin(), doc2.stored.end()
    ));
    writer->commit();
    ASSERT_TRUE(writer->insert(
      doc3.indexed.begin(), doc3.indexed.end(),
      doc3.stored.begin(), doc3.stored.end()
    ));
    ASSERT_TRUE(writer->insert(
      doc4.indexed.begin(), doc4.indexed.end(),
      doc4.stored.begin(), doc4.stored.end()
    ));
    writer->commit();
    writer->remove(std::move(query_doc4.filter));
    writer->commit();
    writer->close();
  }

  auto reader = iresearch::directory_reader::open(dir, codec_ptr);
  iresearch::merge_writer writer(dir, codec_ptr, "merged");

  ASSERT_EQ(2, reader.size());
  ASSERT_EQ(2, reader[0].docs_count());
  ASSERT_EQ(2, reader[1].docs_count());

  // validate initial data (segment 0)
  {
    auto& segment = reader[0];
    ASSERT_EQ(2, segment.docs_count());

    {
      auto fields = segment.fields();
      size_t size = 0;
      while (fields->next()) {
        ++size;
      }
      ASSERT_EQ(7, size);
    }

    // validate bytes field
    {
      auto terms = segment.field("doc_bytes");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto features = tests::binary_field().features();
      features.add<iresearch::norm>();
      std::unordered_map<iresearch::bytes_ref, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("bytes1_data"))].emplace(1);
      expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("bytes2_data"))].emplace(2);

      ASSERT_EQ(2, segment.docs_count("doc_bytes"));
      ASSERT_TRUE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // 'norm' attribute has been specified
      ASSERT_EQ(features, field.features);
      validate_terms(
        *terms,
        2,
        bytes1,
        bytes2,
        2,
        features,
        expected_terms
      );

      std::unordered_map<float_t, iresearch::doc_id_t> expected_values{
        { 1.5f, 1 },
      };

      auto reader = [&expected_values] (iresearch::doc_id_t doc, data_input& in) {
        const auto actual_value = iresearch::read_zvfloat(in); // read norm value

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        expected_values.erase(it);
        return true;
      };
      
      ASSERT_TRUE(segment.visit(field.norm, reader));
      ASSERT_TRUE(expected_values.empty());
    }

    // validate double field
    {
      auto terms = segment.field("doc_double");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto features = tests::double_field().features();
      iresearch::numeric_token_stream max;
      max.reset((double_t) (2.718281828 * 2));
      iresearch::numeric_token_stream min;
      min.reset((double_t) (2.718281828 * 1));
      std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      {
        iresearch::numeric_token_stream itr;
        itr.reset((double_t) (2.718281828 * 1));
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
      }

      {
        iresearch::numeric_token_stream itr;
        itr.reset((double_t) (2.718281828 * 2));
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(2));
      }

      ASSERT_EQ(2, segment.docs_count("doc_double"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      ASSERT_TRUE(max.next() && max.next() && max.next() && max.next()); // skip to last value
      ASSERT_TRUE(min.next()); // skip to first value
      validate_terms(
        *terms,
        2,
        min.attributes().get<iresearch::term_attribute>()->value(),
        max.attributes().get<iresearch::term_attribute>()->value(),
        8,
        features,
        expected_terms
      );
    }

    // validate float field
    {
      auto terms = segment.field("doc_float");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto features = tests::float_field().features();
      iresearch::numeric_token_stream max;
      max.reset((float_t) (3.1415926535 * 2));
      iresearch::numeric_token_stream min;
      min.reset((float_t) (3.1415926535 * 1));
      std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      {
        iresearch::numeric_token_stream itr;
        itr.reset((float_t) (3.1415926535 * 1));
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
      }

      {
        iresearch::numeric_token_stream itr;
        itr.reset((float_t) (3.1415926535 * 2));
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(2));
      }

      ASSERT_EQ(2, segment.docs_count("doc_float"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      ASSERT_TRUE(max.next() && max.next()); // skip to last value
      ASSERT_TRUE(min.next()); // skip to first value
      validate_terms(
        *terms,
        2,
        min.attributes().get<iresearch::term_attribute>()->value(),
        max.attributes().get<iresearch::term_attribute>()->value(),
        4,
        features,
        expected_terms
      );
    }

    // validate int field
    {
      auto terms = segment.field("doc_int");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto features = tests::int_field().features();
      iresearch::numeric_token_stream max;
      max.reset(42 * 2);
      iresearch::numeric_token_stream min;
      min.reset(42 * 1);
      std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      {
        iresearch::numeric_token_stream itr;
        itr.reset(42 * 1);
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
      }

      {
        iresearch::numeric_token_stream itr;
        itr.reset(42 * 2);
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(2));
      }

      ASSERT_EQ(2, segment.docs_count("doc_int"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      ASSERT_TRUE(max.next() && max.next()); // skip to last value
      ASSERT_TRUE(min.next()); // skip to first value
      validate_terms(
        *terms,
        2,
        min.attributes().get<iresearch::term_attribute>()->value(),
        max.attributes().get<iresearch::term_attribute>()->value(),
        3,
        features,
        expected_terms
      );
    }

    // validate long field
    {
      auto terms = segment.field("doc_long");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto features = tests::long_field().features();
      iresearch::numeric_token_stream max;
      max.reset((int64_t) 12345 * 2);
      iresearch::numeric_token_stream min;
      min.reset((int64_t) 12345 * 1);
      std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      {
        iresearch::numeric_token_stream itr;
        itr.reset((int64_t) 12345 * 1);
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
      }

      {
        iresearch::numeric_token_stream itr;
        itr.reset((int64_t) 12345 * 2);
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(2));
      }

      ASSERT_EQ(2, segment.docs_count("doc_long"));
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      ASSERT_TRUE(max.next() && max.next() && max.next() && max.next()); // skip to last value
      ASSERT_TRUE(min.next()); // skip to first value
      validate_terms(
        *terms,
        2,
        min.attributes().get<iresearch::term_attribute>()->value(),
        max.attributes().get<iresearch::term_attribute>()->value(),
        5,
        features,
        expected_terms
      );
    }

    // validate string field
    {
      auto terms = segment.field("doc_string");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto& features = STRING_FIELD_FEATURES;
      size_t frequency = 1;
      std::vector<uint32_t> position = { 0 };
      std::unordered_map<iresearch::bytes_ref, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("string1_data"))].emplace(1);
      expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("string2_data"))].emplace(2);

      ASSERT_EQ(2, segment.docs_count("doc_string"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      validate_terms(
        *terms,
        2,
        iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(string1)),
        iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(string2)),
        2,
        features,
        expected_terms,
        &frequency,
        &position
      );
    }

    // validate text field
    {
      auto terms = segment.field("doc_text");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto& features = TEXT_FIELD_FEATURES;
      size_t frequency = 1;
      std::vector<uint32_t> position = { 0 };
      std::unordered_map<iresearch::bytes_ref, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("text1_data"))].emplace(1);
      expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("text2_data"))].emplace(2);

      ASSERT_EQ(2, segment.docs_count("doc_text"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      validate_terms(
        *terms,
        2,
        iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(text1)),
        iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(text2)),
        2,
        features,
        expected_terms,
        &frequency,
        &position
      );
    }

    // ...........................................................................
    // validate documents
    // ...........................................................................
    std::unordered_set<iresearch::bytes_ref> expected_bytes;
    iresearch::columnstore_reader::value_reader_f bytes_visitor = [&expected_bytes] (data_input& in) {
      const auto value = iresearch::read_string<iresearch::bstring>(in);
      return size_t(1) == expected_bytes.erase(value);
    };
    auto bytes_values = segment.values("doc_bytes", bytes_visitor);
    std::unordered_set<double> expected_double;
    iresearch::columnstore_reader::value_reader_f double_visitor = [&expected_double] (data_input& in) {
      const auto value = iresearch::read_zvdouble(in);
      return size_t(1) == expected_double.erase(value);
    };
    auto double_values = segment.values("doc_double", double_visitor);
    std::unordered_set<float> expected_float;
    iresearch::columnstore_reader::value_reader_f float_visitor = [&expected_float] (data_input& in) {
      const auto value = iresearch::read_zvfloat(in);
      return size_t(1) == expected_float.erase(value);
    };
    auto float_values = segment.values("doc_float", float_visitor);
    std::unordered_set<int> expected_int;
    iresearch::columnstore_reader::value_reader_f int_visitor = [&expected_int] (data_input& in) {
      const auto value = iresearch::read_zvint(in);
      return size_t(1) == expected_int.erase(value);
    };
    auto int_values = segment.values("doc_int", int_visitor);
    std::unordered_set<int64_t> expected_long;
    iresearch::columnstore_reader::value_reader_f long_visitor = [&expected_long] (data_input& in) {
      const auto value = iresearch::read_zvlong(in);
      return size_t(1) == expected_long.erase(value);
    };
    auto long_values = segment.values("doc_long", long_visitor);
    std::unordered_set<std::string> expected_string;
    iresearch::columnstore_reader::value_reader_f string_visitor = [&expected_string] (data_input& in) {
      auto value = iresearch::read_string<std::string>(in);
      return size_t(1) == expected_string.erase(value);
    };
    auto string_values = segment.values("doc_string", string_visitor);

    expected_bytes = { iresearch::bytes_ref(bytes1), iresearch::bytes_ref(bytes2) };
    expected_double = { 2.718281828 * 1, 2.718281828 * 2 };
    expected_float = { (float)(3.1415926535 * 1), (float)(3.1415926535 * 2) };
    expected_int = { 42 * 1, 42 * 2 };
    expected_long = { 12345 * 1, 12345 * 2 };
    expected_string = { string1, string2 };

    // can't have more docs then highest doc_id
    for (size_t i = 0, count = segment.docs_count(); i < count; ++i) {
      const auto doc = iresearch::doc_id_t((iresearch::type_limits<iresearch::type_t::doc_id_t>::min)() + i);
      ASSERT_TRUE(bytes_values(doc));
      ASSERT_TRUE(double_values(doc));
      ASSERT_TRUE(float_values(doc));
      ASSERT_TRUE(int_values(doc));
      ASSERT_TRUE(long_values(doc));
      ASSERT_TRUE(string_values(doc));
    }

    ASSERT_TRUE(expected_bytes.empty());
    ASSERT_TRUE(expected_double.empty());
    ASSERT_TRUE(expected_float.empty());
    ASSERT_TRUE(expected_int.empty());
    ASSERT_TRUE(expected_long.empty());
    ASSERT_TRUE(expected_string.empty());
  }

  // validate initial data (segment 1)
  {
    auto& segment = reader[1];
    ASSERT_EQ(2, segment.docs_count());

    {
      auto fields = segment.fields();
      size_t size = 0;
      while (fields->next()) {
        ++size;
      }
      ASSERT_EQ(7, size);
    }

    // validate bytes field
    {
      auto terms = segment.field("doc_bytes");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto features = tests::binary_field().features();
      std::unordered_map<iresearch::bytes_ref, std::unordered_set<iresearch::doc_id_t>> expected_terms;
      features.add<iresearch::norm>();
      expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("bytes3_data"))].emplace(1);

      ASSERT_EQ(1, segment.docs_count("doc_bytes"));
      ASSERT_TRUE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      validate_terms(
        *terms,
        1,
        bytes3,
        bytes3,
        1,
        features,
        expected_terms
      );
      
      std::unordered_map<float_t, iresearch::doc_id_t> expected_values{
        { 2.5f, 1 },
      };

      auto reader = [&expected_values] (iresearch::doc_id_t doc, data_input& in) {
        const auto actual_value = iresearch::read_zvfloat(in); // read norm value

        auto it = expected_values.find(actual_value);
        if (it == expected_values.end()) {
          // can't find value
          return false;
        }

        if (it->second != doc) {
          // wrong document
          return false;
        }

        expected_values.erase(it);
        return true;
      };
      
      ASSERT_TRUE(segment.visit(field.norm, reader));
      ASSERT_TRUE(expected_values.empty());
    }

    // validate double field
    {
      auto terms = segment.field("doc_double");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto features = tests::double_field().features();
      iresearch::numeric_token_stream max;
      max.reset((double_t) (2.718281828 * 3));
      iresearch::numeric_token_stream min;
      min.reset((double_t) (2.718281828 * 3));
      std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      {
        iresearch::numeric_token_stream itr;
        itr.reset((double_t) (2.718281828 * 3));
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
      }

      ASSERT_EQ(1, segment.docs_count("doc_double"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      ASSERT_TRUE(max.next() && max.next() && max.next() && max.next()); // skip to last value
      ASSERT_TRUE(min.next()); // skip to first value
      validate_terms(
        *terms,
        1,
        min.attributes().get<iresearch::term_attribute>()->value(),
        max.attributes().get<iresearch::term_attribute>()->value(),
        4,
        features,
        expected_terms
      );
    }

    // validate float field
    {
      auto terms = segment.field("doc_float");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto features = tests::float_field().features();
      iresearch::numeric_token_stream max;
      max.reset((float_t) (3.1415926535 * 3));
      iresearch::numeric_token_stream min;
      min.reset((float_t) (3.1415926535 * 3));
      std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      {
        iresearch::numeric_token_stream itr;
        itr.reset((float_t) (3.1415926535 * 3));
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
      }

      ASSERT_EQ(1, segment.docs_count("doc_float"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      ASSERT_TRUE(max.next() && max.next()); // skip to last value
      ASSERT_TRUE(min.next()); // skip to first value
      validate_terms(
        *terms,
        1,
        min.attributes().get<iresearch::term_attribute>()->value(),
        max.attributes().get<iresearch::term_attribute>()->value(),
        2,
        features,
        expected_terms
      );
    }

    // validate int field
    {
      auto terms = segment.field("doc_int");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto features = tests::int_field().features();
      iresearch::numeric_token_stream max;
      max.reset(42 * 3);
      iresearch::numeric_token_stream min;
      min.reset(42 * 3);
      std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      {
        iresearch::numeric_token_stream itr;
        itr.reset(42 * 3);
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
      }

      ASSERT_EQ(1, segment.docs_count("doc_int"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      ASSERT_TRUE(max.next() && max.next()); // skip to last value
      ASSERT_TRUE(min.next()); // skip to first value
      validate_terms(
        *terms,
        1,
        min.attributes().get<iresearch::term_attribute>()->value(),
        max.attributes().get<iresearch::term_attribute>()->value(),
        2,
        features,
        expected_terms
      );
    }

    // validate long field
    {
      auto terms = segment.field("doc_long");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto features = tests::long_field().features();
      iresearch::numeric_token_stream max;
      max.reset((int64_t) 12345 * 3);
      iresearch::numeric_token_stream min;
      min.reset((int64_t) 12345 * 3);
      std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      {
        iresearch::numeric_token_stream itr;
        itr.reset((int64_t) 12345 * 3);
        for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
      }

      ASSERT_EQ(1, segment.docs_count("doc_long"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      ASSERT_TRUE(max.next() && max.next() && max.next() && max.next()); // skip to last value
      ASSERT_TRUE(min.next()); // skip to first value
      validate_terms(
        *terms,
        1,
        min.attributes().get<iresearch::term_attribute>()->value(),
        max.attributes().get<iresearch::term_attribute>()->value(),
        4,
        features,
        expected_terms
      );
    }

    // validate string field
    {
      auto terms = segment.field("doc_string");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto& features = STRING_FIELD_FEATURES;
      size_t frequency = 1;
      std::vector<uint32_t> position = { 0 };
      std::unordered_map<iresearch::bytes_ref, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("string3_data"))].emplace(1);
      expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("string4_data"))];

      ASSERT_EQ(2, segment.docs_count("doc_string"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      validate_terms(
        *terms,
        2,
        iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(string3)),
        iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(string4)),
        2,
        features,
        expected_terms,
        &frequency,
        &position
      );
    }

    // validate text field
    {
      auto terms = segment.field("doc_text");
      ASSERT_NE(nullptr, terms);
      auto& field = terms->meta();
      auto& features = TEXT_FIELD_FEATURES;
      size_t frequency = 1;
      std::vector<uint32_t> position = { 0 };
      std::unordered_map<iresearch::bytes_ref, std::unordered_set<iresearch::doc_id_t>> expected_terms;

      expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("text3_data"))].emplace(1);

      ASSERT_EQ(1, segment.docs_count("doc_text"));
      ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
      ASSERT_EQ(features, field.features);
      ASSERT_NE(nullptr, terms);
      validate_terms(
        *terms,
        1,
        iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(text3)),
        iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(text3)),
        1,
        features,
        expected_terms,
        &frequency,
        &position
      );
    }

    // ...........................................................................
    // validate documents
    // ...........................................................................
    std::unordered_set<iresearch::bytes_ref> expected_bytes;
    iresearch::columnstore_reader::value_reader_f bytes_visitor = [&expected_bytes] (data_input& in) {
      const auto value = iresearch::read_string<iresearch::bstring>(in);
      return size_t(1) == expected_bytes.erase(value);
    };
    auto bytes_values = segment.values("doc_bytes", bytes_visitor);
    std::unordered_set<double> expected_double;
    iresearch::columnstore_reader::value_reader_f double_visitor = [&expected_double] (data_input& in) {
      const auto value = iresearch::read_zvdouble(in);
      return size_t(1) == expected_double.erase(value);
    };
    auto double_values = segment.values("doc_double", double_visitor);
    std::unordered_set<float> expected_float;
    iresearch::columnstore_reader::value_reader_f float_visitor = [&expected_float] (data_input& in) {
      const auto value = iresearch::read_zvfloat(in);
      return size_t(1) == expected_float.erase(value);
    };
    auto float_values = segment.values("doc_float", float_visitor);
    std::unordered_set<int> expected_int;
    iresearch::columnstore_reader::value_reader_f int_visitor = [&expected_int] (data_input& in) {
      const auto value = iresearch::read_zvint(in);
      return size_t(1) == expected_int.erase(value);
    };
    auto int_values = segment.values("doc_int", int_visitor);
    std::unordered_set<int64_t> expected_long;
    iresearch::columnstore_reader::value_reader_f long_visitor = [&expected_long] (data_input& in) {
      const auto value = iresearch::read_zvlong(in);
      return size_t(1) == expected_long.erase(value);
    };
    auto long_values = segment.values("doc_long", long_visitor);
    std::unordered_set<std::string> expected_string;
    iresearch::columnstore_reader::value_reader_f string_visitor = [&expected_string] (data_input& in) {
      auto value = iresearch::read_string<std::string>(in);
      return size_t(1) == expected_string.erase(value);
    };
    auto string_values = segment.values("doc_string", string_visitor);

    expected_bytes = { iresearch::bytes_ref(bytes3) };
    expected_double = { 2.718281828 * 3 };
    expected_float = { (float)(3.1415926535 * 3) };
    expected_int = { 42 * 3 };
    expected_long = { 12345 * 3 };
    expected_string = { string3, string4 };

    // can't have more docs then highest doc_id
    for (size_t i = 0, count = segment.docs_count(); i < count; ++i) {
      const auto doc = iresearch::doc_id_t((iresearch::type_limits<iresearch::type_t::doc_id_t>::min)() + i);
      bytes_values(doc);
      double_values(doc);
      float_values(doc);
      int_values(doc);
      long_values(doc);
      ASSERT_TRUE(string_values(doc));
    }

    ASSERT_TRUE(expected_bytes.empty());
    ASSERT_TRUE(expected_double.empty());
    ASSERT_TRUE(expected_float.empty());
    ASSERT_TRUE(expected_int.empty());
    ASSERT_TRUE(expected_long.empty());
    ASSERT_TRUE(expected_string.empty());
  }

  writer.add(reader[0]);
  writer.add(reader[1]);

  std::string filename;
  iresearch::segment_meta meta;

  writer.flush(filename, meta);

  auto segment = iresearch::segment_reader::open(dir, meta);

  ASSERT_EQ(3, segment.docs_count()); //doc4 removed during merge

  {
    auto fields = segment.fields();
    size_t size = 0;
    while (fields->next()) {
      ++size;
    }
    ASSERT_EQ(7, size);
  }

  // validate bytes field
  {
    auto terms = segment.field("doc_bytes");
    ASSERT_NE(nullptr, terms);
    auto& field = terms->meta();
    auto features = tests::binary_field().features();
    features.add<iresearch::norm>();
    std::unordered_map<iresearch::bytes_ref, std::unordered_set<iresearch::doc_id_t>> expected_terms;

    expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("bytes1_data"))].emplace(1);
    expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("bytes2_data"))].emplace(2);
    expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("bytes3_data"))].emplace(3);

    ASSERT_EQ(3, segment.docs_count("doc_bytes"));
    ASSERT_TRUE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has been specified
    ASSERT_EQ(features, field.features);
    ASSERT_NE(nullptr, terms);
    validate_terms(
      *terms,
      3,
      bytes1,
      bytes3,
      3,
      features,
      expected_terms
    );
      
    std::unordered_map<float_t, iresearch::doc_id_t> expected_values{
      { 1.5f, 1 },
      { 2.5f, 3 },
    };

    auto reader = [&expected_values] (iresearch::doc_id_t doc, data_input& in) {
      const auto actual_value = iresearch::read_zvfloat(in); // read norm value

      auto it = expected_values.find(actual_value);
      if (it == expected_values.end()) {
        // can't find value
        return false;
      }

      if (it->second != doc) {
        // wrong document
        return false;
      }

      expected_values.erase(it);
      return true;
    };

    ASSERT_TRUE(segment.visit(field.norm, reader));
    ASSERT_TRUE(expected_values.empty());
  }

  // validate double field
  {
    auto terms = segment.field("doc_double");
    ASSERT_NE(nullptr, terms);
    auto& field = terms->meta();
    auto features = tests::double_field().features();
    iresearch::numeric_token_stream max;
    max.reset((double_t) (2.718281828 * 3));
    iresearch::numeric_token_stream min;
    min.reset((double_t) (2.718281828 * 1));
    std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

    {
      iresearch::numeric_token_stream itr;
      itr.reset((double_t) (2.718281828 * 1));
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
    }

    {
      iresearch::numeric_token_stream itr;
      itr.reset((double_t) (2.718281828 * 2));
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(2));
    }

    {
      iresearch::numeric_token_stream itr;
      itr.reset((double_t) (2.718281828 * 3));
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(3));
    }

    ASSERT_EQ(3, segment.docs_count("doc_double"));
    ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
    ASSERT_EQ(features, field.features);
    ASSERT_NE(nullptr, terms);
    ASSERT_TRUE(max.next() && max.next() && max.next() && max.next()); // skip to last value
    ASSERT_TRUE(min.next()); // skip to first value
    validate_terms(
      *terms,
      3,
      min.attributes().get<iresearch::term_attribute>()->value(),
      max.attributes().get<iresearch::term_attribute>()->value(),
      12,
      features,
      expected_terms
    );
  }

  // validate float field
  {
    auto terms = segment.field("doc_float");
    ASSERT_NE(nullptr, terms);
    auto& field = terms->meta();
    auto features = tests::float_field().features();
    iresearch::numeric_token_stream max;
    max.reset((float_t) (3.1415926535 * 3));
    iresearch::numeric_token_stream min;
    min.reset((float_t) (3.1415926535 * 1));
    std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

    {
      iresearch::numeric_token_stream itr;
      itr.reset((float_t) (3.1415926535 * 1));
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
    }

    {
      iresearch::numeric_token_stream itr;
      itr.reset((float_t) (3.1415926535 * 2));
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(2));
    }

    {
      iresearch::numeric_token_stream itr;
      itr.reset((float_t) (3.1415926535 * 3));
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(3));
    }

    ASSERT_EQ(3, segment.docs_count("doc_float"));
    ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
    ASSERT_EQ(features, field.features);
    ASSERT_NE(nullptr, terms);
    ASSERT_TRUE(max.next() && max.next()); // skip to last value
    ASSERT_TRUE(min.next()); // skip to first value
    validate_terms(
      *terms,
      3,
      min.attributes().get<iresearch::term_attribute>()->value(),
      max.attributes().get<iresearch::term_attribute>()->value(),
      6,
      features,
      expected_terms
    );
  }

  // validate int field
  {
    auto terms = segment.field("doc_int");
    ASSERT_NE(nullptr, terms);
    auto& field = terms->meta();
    auto features = tests::int_field().features();
    iresearch::numeric_token_stream max;
    max.reset(42 * 3);
    iresearch::numeric_token_stream min;
    min.reset(42 * 1);
    std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

    {
      iresearch::numeric_token_stream itr;
      itr.reset(42 * 1);
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
    }

    {
      iresearch::numeric_token_stream itr;
      itr.reset(42 * 2);
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(2));
    }

    {
      iresearch::numeric_token_stream itr;
      itr.reset(42 * 3);
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(3));
    }

    ASSERT_EQ(3, segment.docs_count("doc_int"));
    ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
    ASSERT_EQ(features, field.features);
    ASSERT_NE(nullptr, terms);
    ASSERT_TRUE(max.next() && max.next()); // skip to last value
    ASSERT_TRUE(min.next()); // skip to first value
    validate_terms(
      *terms,
      3,
      min.attributes().get<iresearch::term_attribute>()->value(),
      max.attributes().get<iresearch::term_attribute>()->value(),
      4,
      features,
      expected_terms
    );
  }

  // validate long field
  {
    auto terms = segment.field("doc_long");
    ASSERT_NE(nullptr, terms);
    auto& field = terms->meta();
    auto features = tests::long_field().features();
    iresearch::numeric_token_stream max;
    max.reset((int64_t) 12345 * 3);
    iresearch::numeric_token_stream min;
    min.reset((int64_t) 12345 * 1);
    std::unordered_map<iresearch::bstring, std::unordered_set<iresearch::doc_id_t>> expected_terms;

    {
      iresearch::numeric_token_stream itr;
      itr.reset((int64_t) 12345 * 1);
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(1));
    }

    {
      iresearch::numeric_token_stream itr;
      itr.reset((int64_t) 12345 * 2);
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(2));
    }

    {
      iresearch::numeric_token_stream itr;
      itr.reset((int64_t) 12345 * 3);
      for (; itr.next(); expected_terms[iresearch::bstring(itr.attributes().get<iresearch::term_attribute>()->value())].emplace(3));
    }

    ASSERT_EQ(3, segment.docs_count("doc_long"));
    ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
    ASSERT_EQ(features, field.features);
    ASSERT_NE(nullptr, terms);
    ASSERT_TRUE(max.next() && max.next() && max.next() && max.next()); // skip to last value
    ASSERT_TRUE(min.next()); // skip to first value
    validate_terms(
      *terms,
      3,
      min.attributes().get<iresearch::term_attribute>()->value(),
      max.attributes().get<iresearch::term_attribute>()->value(),
      6,
      features,
      expected_terms
    );
  }

  // validate string field
  {
    auto terms = segment.field("doc_string");
    ASSERT_NE(nullptr, terms);
    auto& field = terms->meta();
    auto& features = STRING_FIELD_FEATURES;
    size_t frequency = 1;
    std::vector<uint32_t> position = { 0 };
    std::unordered_map<iresearch::bytes_ref, std::unordered_set<iresearch::doc_id_t>> expected_terms;

    expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("string1_data"))].emplace(1);
    expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("string2_data"))].emplace(2);
    expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("string3_data"))].emplace(3);

    ASSERT_EQ(3, segment.docs_count("doc_string"));
    ASSERT_FALSE(iresearch::type_limits<iresearch::type_t::field_id_t>::valid(field.norm)); // norm attribute has not been specified
    ASSERT_EQ(features, field.features);
    ASSERT_NE(nullptr, terms);
    validate_terms(
      *terms,
      3,
      iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(string1)),
      iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(string3)),
      3,
      features,
      expected_terms,
      &frequency,
      &position
    );
  }

  // validate text field
  {
    auto terms = segment.field("doc_text");
    ASSERT_NE(nullptr, terms);
    auto& field = terms->meta();
    auto& features = TEXT_FIELD_FEATURES;
    size_t frequency = 1;
    std::vector<uint32_t> position = { 0 };
    std::unordered_map<iresearch::bytes_ref, std::unordered_set<iresearch::doc_id_t>> expected_terms;

    expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("text1_data"))].emplace(1);
    expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("text2_data"))].emplace(2);
    expected_terms[iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref("text3_data"))].emplace(3);

    ASSERT_EQ(3, segment.docs_count("doc_text"));
    ASSERT_EQ(features, field.features);
    ASSERT_NE(nullptr, terms);
    validate_terms(
      *terms,
      3,
      iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(text1)),
      iresearch::ref_cast<iresearch::byte_type>(iresearch::string_ref(text3)),
      3,
      features,
      expected_terms,
      &frequency,
      &position
    );
  }

  // ...........................................................................
  // validate documents
  // ...........................................................................
  std::unordered_set<iresearch::bytes_ref> expected_bytes;
  iresearch::columnstore_reader::value_reader_f bytes_visitor = [&expected_bytes] (data_input& in) {
    const auto value = iresearch::read_string<iresearch::bstring>(in);
    return size_t(1) == expected_bytes.erase(value);
  };
  auto bytes_values = segment.values("doc_bytes", bytes_visitor);
  std::unordered_set<double> expected_double;
  iresearch::columnstore_reader::value_reader_f double_visitor = [&expected_double] (data_input& in) {
    const auto value = iresearch::read_zvdouble(in);
    return size_t(1) == expected_double.erase(value);
  };
  auto double_values = segment.values("doc_double", double_visitor);
  std::unordered_set<float> expected_float;
  iresearch::columnstore_reader::value_reader_f float_visitor = [&expected_float] (data_input& in) {
    const auto value = iresearch::read_zvfloat(in);
    return size_t(1) == expected_float.erase(value);
  };
  auto float_values = segment.values("doc_float", float_visitor);
  std::unordered_set<int> expected_int;
  iresearch::columnstore_reader::value_reader_f int_visitor = [&expected_int] (data_input& in) {
    const auto value = iresearch::read_zvint(in);
    return size_t(1) == expected_int.erase(value);
  };
  auto int_values = segment.values("doc_int", int_visitor);
  std::unordered_set<int64_t> expected_long;
  iresearch::columnstore_reader::value_reader_f long_visitor = [&expected_long] (data_input& in) {
    const auto value = iresearch::read_zvlong(in);
    return size_t(1) == expected_long.erase(value);
  };
  auto long_values = segment.values("doc_long", long_visitor);
  std::unordered_set<std::string> expected_string;
  iresearch::columnstore_reader::value_reader_f string_visitor = [&expected_string] (data_input& in) {
    auto value = iresearch::read_string<std::string>(in);
    return size_t(1) == expected_string.erase(value);
  };
  auto string_values = segment.values("doc_string", string_visitor);

  expected_bytes = { iresearch::bytes_ref(bytes1), iresearch::bytes_ref(bytes2), iresearch::bytes_ref(bytes3) };
  expected_double = { 2.718281828 * 1, 2.718281828 * 2, 2.718281828 * 3 };
  expected_float = { (float)(3.1415926535 * 1), (float)(3.1415926535 * 2), (float)(3.1415926535 * 3) };
  expected_int = { 42 * 1, 42 * 2, 42 * 3 };
  expected_long = { 12345 * 1, 12345 * 2, 12345 * 3 };
  expected_string = { string1, string2, string3 };

  // can't have more docs then highest doc_id
  for (size_t i = 0, count = segment.docs_count(); i < count; ++i) {
    const auto doc = iresearch::doc_id_t((iresearch::type_limits<iresearch::type_t::doc_id_t>::min)() + i);
    ASSERT_TRUE(bytes_values(doc));
    ASSERT_TRUE(double_values(doc));
    ASSERT_TRUE(float_values(doc));
    ASSERT_TRUE(int_values(doc));
    ASSERT_TRUE(long_values(doc));
    ASSERT_TRUE(string_values(doc));
  }

  ASSERT_TRUE(expected_bytes.empty());
  ASSERT_TRUE(expected_double.empty());
  ASSERT_TRUE(expected_float.empty());
  ASSERT_TRUE(expected_int.empty());
  ASSERT_TRUE(expected_long.empty());
  ASSERT_TRUE(expected_string.empty());
}

TEST_F(merge_writer_tests, test_merge_writer_field_features) {
  //iresearch::flags STRING_FIELD_FEATURES{ iresearch::frequency::type(), iresearch::position::type() };
  //iresearch::flags TEXT_FIELD_FEATURES{ iresearch::frequency::type(), iresearch::position::type(), iresearch::offset::type(), iresearch::payload::type() };

  std::string field("doc_string");
  std::string data("string_data");
  tests::document doc1; // string
  tests::document doc2; // text

  doc1.insert(std::make_shared<tests::templates::string_field>(field, data));
  doc2.indexed.push_back(std::make_shared<tests::templates::text_field<iresearch::string_ref>>(field, data, true));

  ASSERT_TRUE(doc1.indexed.get(field)->features().is_subset_of(doc2.indexed.get(field)->features()));
  ASSERT_FALSE(doc2.indexed.get(field)->features().is_subset_of(doc1.indexed.get(field)->features()));

  iresearch::version10::format codec;
  iresearch::format::ptr codec_ptr(&codec, [](iresearch::format*)->void{});
  iresearch::memory_directory dir;

  // populate directory
  {
    auto writer = iresearch::index_writer::make(dir, codec_ptr, iresearch::OM_CREATE);
    ASSERT_TRUE(writer->insert(
      doc1.indexed.begin(), doc1.indexed.end(),
      doc1.stored.begin(), doc1.stored.end()
    ));
    writer->commit();
    ASSERT_TRUE(writer->insert(
      doc2.indexed.begin(), doc2.indexed.end(),
      doc2.stored.begin(), doc2.stored.end()
    ));
    writer->commit();
    writer->close();
  }

  auto reader = iresearch::directory_reader::open(dir, codec_ptr);

  ASSERT_EQ(2, reader.size());
  ASSERT_EQ(1, reader[0].docs_count());
  ASSERT_EQ(1, reader[1].docs_count());

  // test merge existing with feature subset (success)
  {
    iresearch::merge_writer writer(dir, codec_ptr, "merged_subset");
    writer.add(reader[1]); // assume 1 is segment with text field
    writer.add(reader[0]); // assume 0 is segment with string field

    std::string filename;
    iresearch::segment_meta meta;

    ASSERT_TRUE(writer.flush(filename, meta));
  }

  // test merge existing with feature superset (fail)
  {
    iresearch::merge_writer writer(dir, codec_ptr, "merged_superset");
    writer.add(reader[0]); // assume 0 is segment with text field
    writer.add(reader[1]); // assume 1 is segment with string field

    std::string filename;
    iresearch::segment_meta meta;

    ASSERT_FALSE(writer.flush(filename, meta));
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------