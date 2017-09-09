////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#ifndef IRESEARCH_BITSET_DOC_ITERATOR_H
#define IRESEARCH_BITSET_DOC_ITERATOR_H

#include "index/iterators.hpp"
#include "utils/type_limits.hpp"
#include "cost.hpp"

NS_ROOT

class bitset;

class bitset_doc_iterator final : public score_doc_iterator, util::noncopyable {
 public:
  explicit bitset_doc_iterator(const bitset& set);

  virtual doc_id_t value() const NOEXCEPT override {
    return doc_;
  }

  virtual bool next() NOEXCEPT override;

  virtual doc_id_t seek(doc_id_t target) NOEXCEPT override;

  virtual const attribute_view& attributes() const NOEXCEPT override {
    return attrs_;
  }

  virtual void score() NOEXCEPT override { }

 private:
  doc_id_t next_from(doc_id_t target) NOEXCEPT;

  attribute_view attrs_;
  doc_id_t doc_{ type_limits<type_t::doc_id_t>::invalid() };
  cost est_;
  const bitset& set_;
}; // bitset_doc_iterator

NS_END // ROOT

#endif // IRESEARCH_BITSET_DOC_ITERATOR_H

