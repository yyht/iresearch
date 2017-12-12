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

#include "composite_reader_impl.hpp"
#include "search/bitset_doc_iterator.hpp"
#include "store/store_utils.hpp"

#include "transaction_store.hpp"

NS_LOCAL

static const size_t DEFAULT_BUFFER_SIZE = 512; // arbitrary value

// state at start of each unique column
struct column_stats_t {
  size_t offset = 0; // offset to previous column data start in buffer
};

// state at start of each unique document (only once since state is per-document)
struct doc_stats_t {
  float_t norm = irs::norm::DEFAULT();
  uint32_t term_count = 0; // number of terms in a document
};

// state at start of each unique field (for all terms)
struct field_stats_t {
  float_t boost = 1.f;
  uint32_t pos = irs::integer_traits<uint32_t>::const_max; // current term position
  uint32_t pos_last = 0; // previous term position
  uint32_t max_term_freq = 0; // highest term frequency in a field across all terms/documents
  uint32_t num_overlap = 0; // number of overlapped terms
  uint32_t offs_start_base = 0; // current field term-offset base (i.e. previous same-field last term-offset-end) for computation of absolute offset
  uint32_t offs_start_term = 0; // current field previous term-offset start (offsets may overlap)
  uint32_t unq_term_count = 0; // number of unique terms
};

// state at start of each unique term
struct term_stats_t {
  size_t offset = 0; // offset to previous term data start in buffer
  uint32_t term_freq = 0; // term frequency in a document
};

struct document_less_t {
  typedef irs::transaction_store::document_t document_t;

  bool operator() (const document_t& doc, irs::doc_id_t id) const {
    return doc.doc_id_ < id;
  }

  bool operator() (irs::doc_id_t id, const document_t& doc) const {
    return id < doc.doc_id_;
  }

  bool operator() (const document_t& lhs, const document_t& rhs) const {
    return lhs.doc_id_ < rhs.doc_id_;
  }
};

const document_less_t DOC_LESS;

struct empty_seek_term_iterator: public irs::seek_term_iterator {
  virtual const irs::attribute_view& attributes() const NOEXCEPT override {
    return irs::attribute_view::empty_instance();
  }

  virtual bool next() override { return false; }

  virtual irs::doc_iterator::ptr postings(const irs::flags&) const override {
    return irs::doc_iterator::empty();
  }

  virtual void read() override { }
  virtual bool seek(const irs::bytes_ref& value) override { return false; }

  virtual bool seek(
      const irs::bytes_ref& term, const seek_cookie& cookie
  ) override {
    return false;
  }

  virtual seek_cookie::ptr cookie() const { return nullptr; }

  virtual irs::SeekResult seek_ge(const irs::bytes_ref& value) {
    return irs::SeekResult::END;
  }

  virtual const irs::bytes_ref& value() const override {
    return irs::bytes_ref::nil;
  }
};

class single_reader_iterator_impl final
    : public irs::index_reader::reader_iterator_impl {
 public:
  DECLARE_PTR(single_reader_iterator_impl); // required for PTR_NAMED(...)

  explicit single_reader_iterator_impl(
      const irs::sub_reader* reader = nullptr
  ) NOEXCEPT: reader_(reader) {
  }

  virtual void operator++() override { reader_ = nullptr; }

  virtual reference operator*() override {
    return *const_cast<irs::sub_reader*>(reader_);
  }

  virtual const_reference operator*() const override { return *reader_; }

  virtual bool operator==(
    const irs::index_reader::reader_iterator_impl& rhs
  ) override {
    return reader_ == static_cast<const single_reader_iterator_impl&>(rhs).reader_;
  }

 private:
  const iresearch::sub_reader* reader_;
};

template<typename M>
class store_column_iterator final: public irs::column_iterator {
 public:
  store_column_iterator(const M& map)
    : itr_(map.begin()), map_(map), value_(nullptr) {}

  virtual bool next() override {
    if (map_.end() == itr_) {
      value_ = nullptr;

      return false; // already at end
    }

    value_ = itr_->second.meta_.get();
    ++itr_;

    return true;
  }

  virtual bool seek(const irs::string_ref& name) override {
    itr_ = map_.lower_bound(name);

    return next();
  }

  virtual const irs::column_meta& value() const override {
    static const irs::column_meta invalid;

    return value_ ? *value_ : invalid;
  }

 private:
  typename M::const_iterator itr_;
  const M& map_;
  const irs::column_meta* value_;
};

template<typename M>
class store_field_iterator final: public irs::field_iterator {
 public:
  store_field_iterator(const M& map)
    : itr_(map.begin()), map_(map), value_(nullptr) {}

  virtual bool next() override {
    if (map_.end() == itr_) {
      value_ = nullptr;

      return false; // already at end
    }

    value_ = &(itr_->second);
    ++itr_;

    return true;
  };

  virtual bool seek(const irs::string_ref& name) override {
    itr_ = map_.lower_bound(name);

    return next();
  }

  virtual const irs::term_reader& value() const override {
    assert(value_);

    return *value_;
  }

 private:
  typename M::const_iterator itr_;
  const M& map_;
  const irs::term_reader* value_;
};

// ----------------------------------------------------------------------------
// --SECTION--                                      store_reader implementation
// ----------------------------------------------------------------------------

class store_reader_impl final: public irs::sub_reader {
 public:
  typedef irs::transaction_store::document_entry_t document_entry_t;
  typedef std::vector<document_entry_t> document_entries_t;

  struct column_reader_t: public irs::columnstore_reader::column_reader {
    document_entries_t entries_;
    column_reader_t(document_entries_t&& entries) NOEXCEPT
      : entries_(std::move(entries)) {}
    virtual irs::columnstore_iterator::ptr iterator() const override;
    virtual size_t size() const override { return entries_.size(); }
    virtual irs::columnstore_reader::values_reader_f values() const override;
    virtual bool visit(
      const irs::columnstore_reader::values_visitor_f& visitor
    ) const override;
  };

  struct named_column_reader_t: public column_reader_t {
    const irs::transaction_store::column_meta_builder::ptr meta_; // copy from 'store' because column in store may disapear
    named_column_reader_t(
        const irs::transaction_store::column_meta_builder::ptr& meta,
        document_entries_t&& entries
    ): column_reader_t(std::move(entries)), meta_(meta) { assert(meta_); }
  };

  typedef std::map<irs::string_ref, named_column_reader_t> columns_named_t;
  typedef std::map<irs::field_id, column_reader_t> columns_unnamed_t;

  struct term_entry_t {
    document_entries_t entries_;
    irs::term_meta meta_; // copy from 'store' because term in store may disapear
    const irs::transaction_store::bstring_builder::ptr name_; // copy from 'store' because term in store may disapear
    term_entry_t(
        const irs::transaction_store::bstring_builder::ptr& name,
        const irs::term_meta& meta,
        document_entries_t&& entries
    ): entries_(std::move(entries)), meta_(meta), name_(name) { assert(name_); }
  };

  typedef std::map<irs::bytes_ref, term_entry_t> term_entries_t;

  struct term_reader_t: public irs::term_reader {
    irs::attribute_view attrs_;
    uint64_t doc_count_;
    irs::bytes_ref max_term_;
    const irs::transaction_store::field_meta_builder::ptr meta_; // copy from 'store' because field in store may disapear
    irs::bytes_ref min_term_;
    term_entries_t terms_;

    term_reader_t(const irs::transaction_store::field_meta_builder::ptr& meta)
      : meta_(meta) { assert(meta_); }
    term_reader_t(term_reader_t&& other) NOEXCEPT;
    virtual const irs::attribute_view& attributes() const NOEXCEPT override {
      return attrs_;
    }
    virtual uint64_t docs_count() const override { return doc_count_; }
    virtual irs::seek_term_iterator::ptr iterator() const override;
    virtual const irs::bytes_ref& max() const override { return max_term_; }
    virtual const irs::field_meta& meta() const override { return *meta_; }
    virtual const irs::bytes_ref& min() const override { return min_term_; }
    virtual size_t size() const override { return terms_.size(); }
  };

  typedef std::map<irs::string_ref, term_reader_t> fields_t;

  virtual index_reader::reader_iterator begin() const override;
  virtual const irs::column_meta* column(const irs::string_ref& name) const override;
  virtual irs::column_iterator::ptr columns() const override;
  virtual const irs::columnstore_reader::column_reader* column_reader(irs::field_id field) const override;
  virtual uint64_t docs_count() const override { return documents_.size(); } // +1 for invalid doc if non empty
  virtual docs_iterator_t::ptr docs_iterator() const override;
  virtual index_reader::reader_iterator end() const override;
  virtual const irs::term_reader* field(const irs::string_ref& field) const override;
  virtual irs::field_iterator::ptr fields() const override;
  virtual uint64_t live_docs_count() const override { return documents_.count(); }
  virtual size_t size() const override { return 1; } // only 1 segment

 private:
  friend irs::store_reader irs::store_reader::reopen() const;
  friend irs::store_reader irs::transaction_store::reader() const;
  friend bool irs::transaction_store::flush(irs::index_writer&); // to allow use of private constructor
  typedef std::unordered_map<irs::field_id, const column_reader_t*> column_by_id_t;

  const columns_named_t columns_named_;
  const columns_unnamed_t columns_unnamed_;
  const column_by_id_t column_by_id_;
  const irs::bitvector documents_;
  const fields_t fields_;
  const size_t generation_;
  const irs::transaction_store& store_;

  store_reader_impl(
    const irs::transaction_store& store,
    irs::bitvector&& documents,
    fields_t&& fields,
    columns_named_t&& columns_named,
    columns_unnamed_t&& columns_unnamed,
    size_t generation
  );
};

class store_col_iterator: public irs::columnstore_iterator {
 public:
  store_col_iterator(const store_reader_impl::document_entries_t& entries)
    : entry_(nullptr),
      entries_(entries),
      next_itr_(entries.begin()),
      next_offset_(EOFOFFSET),
      value_(INVALID) {
  }

  virtual bool next() override {
    do {
      if (next_itr_ == entries_.end()) {
        entry_ = nullptr;
        next_offset_ = EOFOFFSET;
        value_ = EOFMAX; // invalid data size

        return false;
      }

      entry_ = &*next_itr_;
      next_offset_ = next_itr_->offset_;
      value_.first = next_itr_->doc_id_;
      ++next_itr_;
    } while (!(entry_->buf_) || !next_value()); // skip invalid doc ids

    return true;
  }

  bool next_value() {
    if (!entry_ || next_offset_ == EOFOFFSET) {
      return false;
    }

    irs::bytes_ref_input in(*(entry_->buf_)); // buf_ validity checked in next()
    auto offset = next_offset_;

    in.seek(offset);
    next_offset_ = in.read_long(); // read next value offset

    auto size = in.read_vlong(); // read value size
    auto start = offset - size;

    if (offset < size) {
      next_offset_ = EOFOFFSET;
      value_ = EOFMAX; // invalid data size

      return false;
    }

    value_.second = irs::bytes_ref(entry_->buf_->data() + start, size);

    return true;
  }

  virtual const value_type& seek(irs::doc_id_t doc) override {
    next_itr_ = std::lower_bound(entries_.begin(), entries_.end(), doc, DOC_LESS);
    next();

    return value();
  }

  virtual const value_type& value() const override { return value_; }

 private:
  static const columnstore_iterator::value_type EOFMAX;
  static const size_t EOFOFFSET = irs::integer_traits<size_t>::const_max;
  static const columnstore_iterator::value_type INVALID;

  const store_reader_impl::document_entry_t* entry_;
  const store_reader_impl::document_entries_t& entries_;
  store_reader_impl::document_entries_t::const_iterator next_itr_;
  size_t next_offset_;
  value_type value_;
};

/*static*/ const irs::columnstore_iterator::value_type store_col_iterator::EOFMAX {
  irs::type_limits<irs::type_t::doc_id_t>::eof(),
  irs::bytes_ref::nil
};

/*static*/ const irs::columnstore_iterator::value_type store_col_iterator::INVALID {
  irs::type_limits<irs::type_t::doc_id_t>::invalid(),
  irs::bytes_ref::nil
};

class store_doc_iterator: public irs::doc_iterator {
 public:
  store_doc_iterator(
      const store_reader_impl::document_entries_t& entries,
      const irs::flags& field_features,
      const irs::flags& requested_features
  ): entry_(nullptr),
     entries_(entries),
     load_frequency_(requested_features.check<irs::frequency>()),
     next_itr_(entries.begin()) {
    attrs_.emplace(doc_);
    doc_.value = irs::type_limits<irs::type_t::doc_id_t>::invalid();

    if (load_frequency_) {
      attrs_.emplace(doc_freq_);
    }

    if (requested_features.check<irs::position>()
        && field_features.check<irs::position>()) {
      attrs_.emplace(doc_pos_);
      doc_pos_.reset(irs::memory::make_unique<position_t>(
        field_features, requested_features, entry_
      ));
    }
  }

  virtual const irs::attribute_view& attributes() const NOEXCEPT override {
    return attrs_;
  }

  bool load_attributes() {
    if (!load_frequency_ && !doc_pos_) {
      return true; // nothing to do
    }

    if (!(entry_->buf_)) {
      return false;
    }

    // @see store_writer::index(...) for format definition
    auto next = entry_->offset_;

    doc_freq_.value = 0; // reset for new entry

    // length of linked list == term frequency in current document
    for (irs::bytes_ref_input in(*(entry_->buf_)); next; next = in.read_long()) {
      ++doc_freq_.value;
      in.seek(next);
    }

    if (doc_pos_) {
      doc_pos_.clear(); // reset impl to new doc
    }

    return true;
  }

  virtual bool next() override {
    do {
      if (next_itr_ == entries_.end()) {
        entry_ = nullptr;
        doc_.value = irs::type_limits<irs::type_t::doc_id_t>::eof();

        return false;
      }

      entry_ = &*next_itr_;
      doc_.value = entry_->doc_id_;
      ++next_itr_;
    } while (!load_attributes()); // skip invalid doc ids

    return true;
  }

  virtual irs::doc_id_t seek(irs::doc_id_t doc) override {
    next_itr_ = std::lower_bound(entries_.begin(), entries_.end(), doc, DOC_LESS);
    next();

    return value();
  }

  virtual irs::doc_id_t value() const override { return doc_.value; }

 private:
  struct position_t: public irs::position::impl {
    const store_reader_impl::document_entry_t*& entry_;
    bool has_offs_;
    bool has_pay_;
    size_t next_;
    irs::offset offs_;
    irs::position::value_t pos_;
    irs::payload pay_;

    position_t(
        const irs::flags& field_features,
        const irs::flags& requested_features,
        const store_reader_impl::document_entry_t*& entry
    ): entry_(entry),
       has_offs_(field_features.check<irs::offset>()),
       has_pay_(field_features.check<irs::payload>()) {
      if (has_offs_ && requested_features.check<irs::offset>()) {
        attrs_.emplace(offs_);
      }

      if (has_pay_ && requested_features.check<irs::payload>()) {
        attrs_.emplace(pay_);
      }

      clear();
    }

    virtual void clear() override {
      next_ = entry_ ? entry_->offset_ : 0; // 0 indicates end of list in format definition
      offs_.clear();
      pos_ = irs::position::INVALID;
      pay_.clear();
    }

    virtual irs::position::value_t value() const override { return pos_; }

    virtual bool next() override {
      if (!next_ || !entry_ || !entry_->buf_ || next_ >= entry_->buf_->size()) {
        next_ = 0; // 0 indicates end of list in format definition
        offs_.clear();
        pos_ = irs::position::INVALID;
        pay_.clear();

        return false;
      }

      // @see store_writer::index(...) for format definition
      irs::bytes_ref_input in(*(entry_->buf_));

      in.seek(next_);
      next_ = in.read_long(); // read 'next-pointer'
      pos_ = read_zvint(in);

      if (has_offs_) {
        offs_.start = read_zvint(in);
        offs_.end = read_zvint(in);
      }

      pay_.value = !in.read_byte()
        ? irs::bytes_ref::nil
        : irs::to_string<irs::bytes_ref>(entry_->buf_->data() + in.file_pointer());
        ;

      return true;
    }
  };

  irs::attribute_view attrs_;
  irs::document doc_;
  irs::frequency doc_freq_;
  irs::position doc_pos_;
  const store_reader_impl::document_entry_t* entry_;
  const store_reader_impl::document_entries_t& entries_;
  bool load_frequency_; // should the frequency attribute be updated (optimization)
  store_reader_impl::document_entries_t::const_iterator next_itr_;
};

class store_term_iterator: public irs::seek_term_iterator {
 public:
  store_term_iterator(
      const irs::flags& field_features,
      const store_reader_impl::term_entries_t& terms
  ): field_features_(field_features),
     term_entry_(nullptr),
     attrs_(2), // term_meta + frequency
     next_itr_(terms.begin()),
     terms_(terms) {
    attrs_.emplace(meta_);

    if (field_features_.check<irs::frequency>()) {
      attrs_.emplace(freq_);
    }
  }

  const irs::attribute_view& attributes() const NOEXCEPT override {
    return attrs_;
  }

  virtual seek_term_iterator::seek_cookie::ptr cookie() const override {
    return irs::memory::make_unique<cookie_t>(terms_.find(value()));
  }

  virtual bool next() override {
    if (next_itr_ == terms_.end()) {
      term_ = irs::bytes_ref::nil;
      term_entry_ = nullptr;

      return false;
    }

    term_ = next_itr_->first;
    term_entry_ = &(next_itr_->second);
    ++next_itr_;

    return true;
  }

  virtual irs::doc_iterator::ptr postings(
      const irs::flags& features
  ) const override {
    return !term_entry_ || term_entry_->entries_.empty()
      ? irs::doc_iterator::empty()
      : irs::doc_iterator::make<store_doc_iterator>(
          term_entry_->entries_,
          field_features_,
          features
        )
      ;
  }

  virtual void read() override {
    if (!term_entry_) {
      return; // nothing to do
    }

    freq_.value = term_entry_->meta_.freq;
    meta_ = term_entry_->meta_;
  }

  virtual bool seek(const irs::bytes_ref& term) override {
    return irs::SeekResult::FOUND == seek_ge(term);
  }

  virtual bool seek(
    const irs::bytes_ref& term,
    const irs::seek_term_iterator::seek_cookie& cookie
  ) override {
    #ifdef IRESEARCH_DEBUG
      const auto& state = dynamic_cast<const cookie_t&>(cookie);
    #else
      const auto& state = static_cast<const cookie_t&>(cookie);
    #endif

    next_itr_ = state.itr_;

    return next();
  }

  virtual irs::SeekResult seek_ge(const irs::bytes_ref& term) override {
    next_itr_ = terms_.lower_bound(term);

    if (!next()) {
      return irs::SeekResult::END;
    }

    return value() == term
      ? irs::SeekResult::FOUND
      : irs::SeekResult::NOT_FOUND
      ;
  }

  virtual const irs::bytes_ref& value() const override { return term_; }

 protected:
  const irs::flags& field_features_;
  const store_reader_impl::term_entry_t* term_entry_;

 private:
  struct cookie_t final: public irs::seek_term_iterator::seek_cookie {
    store_reader_impl::term_entries_t::const_iterator itr_;
    cookie_t(const store_reader_impl::term_entries_t::const_iterator& itr)
      : itr_(itr) {}
  };

  irs::attribute_view attrs_;
  irs::frequency freq_;
  irs::term_meta meta_;
  store_reader_impl::term_entries_t::const_iterator next_itr_;
  irs::bytes_ref term_;
  const store_reader_impl::term_entries_t& terms_;
};

irs::columnstore_iterator::ptr store_reader_impl::column_reader_t::iterator() const {
  return entries_.empty()
    ? irs::columnstore_reader::empty_iterator()
    : irs::columnstore_iterator::make<store_col_iterator>(entries_);
}

irs::columnstore_reader::values_reader_f store_reader_impl::column_reader_t::values() const {
  if (entries_.empty()) {
    return irs::columnstore_reader::empty_reader();
  }

  return [this](irs::doc_id_t key, irs::bytes_ref& value)->bool {
    auto itr = std::lower_bound(entries_.begin(), entries_.end(), key, DOC_LESS);

    if (itr == entries_.end() || itr->doc_id_ != key || !itr->buf_) {
      return false; // no entry >= doc
    }

    auto& buf = *(itr->buf_);
    irs::bytes_ref_input in(buf);

    in.seek(itr->offset_);
    in.read_long(); // read next value offset

    auto size = in.read_vlong(); // read value size
    auto start = itr->offset_ - size;

    if (itr->offset_ < size) {
      return false; // invalid data size
    }

    value = irs::bytes_ref(buf.data() + start, size);

    return true;
  };
}

bool store_reader_impl::column_reader_t::visit(
    const irs::columnstore_reader::values_visitor_f& visitor
) const {
  for (auto& entry: entries_) {
    if (!(entry.buf_)) {
      continue;
    }

    auto doc_id = entry.doc_id_;
    irs::bytes_ref_input in(*(entry.buf_));

    for(auto next_offset = entry.offset_; next_offset;) {
      auto offset = next_offset;

      in.seek(next_offset);
      next_offset = in.read_long(); // read next value offset

      auto size = in.read_vlong(); // read value size
      auto start = offset - size;

      if (offset < size) {
        break; // invalid data size
      }

      if (!visitor(doc_id, irs::bytes_ref(entry.buf_->data() + start, size))) {
        return false;
      }
    }
  }

  return true;
}

store_reader_impl::term_reader_t::term_reader_t(term_reader_t&& other) NOEXCEPT
  : attrs_(std::move(other.attrs_)),
    doc_count_(std::move(other.doc_count_)),
    max_term_(std::move(other.max_term_)),
    meta_(std::move(other.meta_)),
    min_term_(std::move(other.min_term_)),
    terms_(std::move(other.terms_)) {
}

irs::seek_term_iterator::ptr store_reader_impl::term_reader_t::iterator() const {
  return terms_.empty()
    ? irs::seek_term_iterator::make<empty_seek_term_iterator>()
    : irs::seek_term_iterator::make<store_term_iterator>(meta_->features, terms_);
}

store_reader_impl::store_reader_impl(
    const irs::transaction_store& store,
    irs::bitvector&& documents,
    fields_t&& fields,
    columns_named_t&& columns_named,
    columns_unnamed_t&& columns_unnamed,
    size_t generation
): columns_named_(std::move(columns_named)),
   columns_unnamed_(std::move(columns_unnamed)),
   documents_(std::move(documents)),
   fields_(std::move(fields)),
   generation_(generation),
   store_(store) {
  auto& column_by_id = const_cast<column_by_id_t&>(column_by_id_); // initialize map

  for (auto& entry: columns_named_) {
    auto& column = entry.second;

    column_by_id.emplace(column.meta_->id, &column);
  }

  for (auto& entry: columns_unnamed_) {
    column_by_id.emplace(entry.first, &(entry.second));
  }
}

irs::index_reader::reader_iterator store_reader_impl::begin() const {
  PTR_NAMED(single_reader_iterator_impl, itr, this);

  return index_reader::reader_iterator(itr.release());
}

const irs::column_meta* store_reader_impl::column(
    const irs::string_ref& name
) const {
  auto itr = columns_named_.find(name);

  return itr == columns_named_.end() ? nullptr : itr->second.meta_.get();
}

irs::column_iterator::ptr store_reader_impl::columns() const {
  auto ptr = irs::memory::make_unique<store_column_iterator<columns_named_t>>(columns_named_);

  return irs::memory::make_managed<irs::column_iterator, true>(std::move(ptr));
}

const irs::columnstore_reader::column_reader* store_reader_impl::column_reader(
    irs::field_id field
) const {
  auto itr = column_by_id_.find(field);

  return itr == column_by_id_.end() ? nullptr : itr->second;
}

irs::sub_reader::docs_iterator_t::ptr store_reader_impl::docs_iterator() const {
  auto ptr =
    irs::memory::make_unique<irs::bitset_doc_iterator>(
      *this,
      irs::attribute_store::empty_instance(),
      documents_,
      irs::order::prepared::unordered()
    );

  return ptr;
}

irs::index_reader::reader_iterator store_reader_impl::end() const {
  PTR_NAMED(single_reader_iterator_impl, itr);

  return index_reader::reader_iterator(itr.release());
}

const irs::term_reader* store_reader_impl::field(
    const irs::string_ref& field
) const {
  auto itr = fields_.find(field);

  return itr == fields_.end() ? nullptr : &(itr->second);
}

irs::field_iterator::ptr store_reader_impl::fields() const {
  auto ptr = irs::memory::make_unique<store_field_iterator<fields_t>>(fields_);

  return irs::memory::make_managed<irs::field_iterator, true>(std::move(ptr));
}

// ----------------------------------------------------------------------------
// --SECTION--                              masking_store_reader implementation
// ----------------------------------------------------------------------------

class masking_store_reader final: public irs::sub_reader {
 public:
  typedef store_reader_impl::document_entries_t document_entries_t;

  struct column_reader_t: public store_reader_impl::column_reader_t {
    const irs::bitvector* documents_;
    column_reader_t(document_entries_t&& entries) NOEXCEPT
      : store_reader_impl::column_reader_t(std::move(entries)), documents_(nullptr) {}
    virtual irs::columnstore_iterator::ptr iterator() const override;
    virtual irs::columnstore_reader::values_reader_f values() const override;
    virtual bool visit(
      const irs::columnstore_reader::values_visitor_f& visitor
    ) const override;
  };

  struct named_column_reader_t: public column_reader_t {
    const irs::transaction_store::column_meta_builder::ptr meta_;
    named_column_reader_t(
        const irs::transaction_store::column_meta_builder::ptr& meta,
        document_entries_t&& entries
    ): column_reader_t(std::move(entries)), meta_(meta) { assert(meta_); }
  };

  typedef std::map<irs::string_ref, named_column_reader_t> columns_named_t;
  typedef std::map<irs::field_id, column_reader_t> columns_unnamed_t;
  typedef store_reader_impl::term_entry_t term_entry_t;

  struct term_reader_t: public store_reader_impl::term_reader_t {
    const irs::bitvector* documents_;

    term_reader_t(const irs::transaction_store::field_meta_builder::ptr& meta)
      : store_reader_impl::term_reader_t(meta), documents_(nullptr) {}
    virtual irs::seek_term_iterator::ptr iterator() const override;
  };

  typedef std::map<irs::string_ref, term_reader_t> fields_t;

  masking_store_reader(
      const irs::bitvector& documents,
      fields_t&& fields,
      columns_named_t&& columns_named,
      columns_unnamed_t&& columns_unnamed
  );
  virtual index_reader::reader_iterator begin() const override;
  virtual const irs::column_meta* column(const irs::string_ref& name) const override;
  virtual irs::column_iterator::ptr columns() const override;
  virtual const irs::columnstore_reader::column_reader* column_reader(irs::field_id field) const override;
  virtual uint64_t docs_count() const override { return documents_.size(); } // +1 for invalid doc if non empty
  virtual docs_iterator_t::ptr docs_iterator() const override;
  virtual index_reader::reader_iterator end() const override;
  virtual const irs::term_reader* field(const irs::string_ref& field) const override;
  virtual irs::field_iterator::ptr fields() const override;
  virtual uint64_t live_docs_count() const override { return documents_.count(); }
  virtual size_t size() const override { return 1; } // only 1 segment

 private:
  typedef std::unordered_map<irs::field_id, const column_reader_t*> column_by_id_t;

  const columns_named_t columns_named_;
  const columns_unnamed_t columns_unnamed_;
  const column_by_id_t column_by_id_;
  const irs::bitvector& documents_;
  const fields_t fields_;
};

class masking_store_col_iterator: public store_col_iterator {
 public:
  masking_store_col_iterator(
      const irs::bitvector& documents,
      const store_reader_impl::document_entries_t& entries
  ): store_col_iterator(entries), documents_(documents) {}

  virtual bool next() override {
    while (store_col_iterator::next()) {
      if (documents_.test(value().first)) {
        return true;
      }
    }

    return false;
  }

 private:
  const irs::bitvector& documents_;
};

class masking_store_doc_iterator: public store_doc_iterator {
 public:
  masking_store_doc_iterator(
      const irs::bitvector& documents,
      const store_reader_impl::document_entries_t& entries,
      const irs::flags& field_features,
      const irs::flags& requested_features
  ): store_doc_iterator(entries, field_features, requested_features),
     documents_(documents) {
  }

  virtual bool next() override {
    while (store_doc_iterator::next()) {
      if (documents_.test(value())) {
        return true;
      }
    }

    return false;
  }

 private:
  const irs::bitvector& documents_;
};

class masking_store_term_iterator: public store_term_iterator {
 public:
  masking_store_term_iterator(
      const irs::bitvector& documents,
      const irs::flags& field_features,
      const store_reader_impl::term_entries_t& terms
  ): store_term_iterator(field_features, terms), documents_(documents) {}

  virtual irs::doc_iterator::ptr postings(
      const irs::flags& features
  ) const override {
    return !term_entry_ || term_entry_->entries_.empty()
      ? irs::doc_iterator::empty()
      : irs::doc_iterator::make<masking_store_doc_iterator>(
          documents_,
          term_entry_->entries_,
          field_features_,
          features
        )
      ;
  }

 private:
  const irs::bitvector& documents_;
};

irs::columnstore_iterator::ptr masking_store_reader::column_reader_t::iterator() const {
  return !documents_ || entries_.empty()
    ? irs::columnstore_reader::empty_iterator()
    : irs::columnstore_iterator::make<masking_store_col_iterator>(*documents_, entries_);
}

irs::columnstore_reader::values_reader_f masking_store_reader::column_reader_t::values() const {
  if (!documents_ || entries_.empty()) {
    return irs::columnstore_reader::empty_reader();
  }

  auto& documents = *documents_;
  auto reader = store_reader_impl::column_reader_t::values();

  return [&documents, reader](irs::doc_id_t key,irs::bytes_ref& value)->bool {
    return documents.test(key) && reader(key, value);
  };
}

bool masking_store_reader::column_reader_t::visit(
  const irs::columnstore_reader::values_visitor_f& visitor
) const {
  if (!documents_) {
    return false;
  }

  for (auto& entry: entries_) {
    if (!(entry.buf_) || !documents_->test(entry.doc_id_)) {
      continue;
    }

    irs::bytes_ref_input in(*(entry.buf_));

    for(auto next_offset = entry.offset_; next_offset;) {
      auto offset = next_offset;

      in.seek(next_offset);
      next_offset = in.read_long(); // read next value offset

      auto size = in.read_vlong(); // read value size
      auto start = offset - size;

      if (offset < size) {
        break; // invalid data size
      }

      if (!visitor(entry.doc_id_, irs::bytes_ref(entry.buf_->data() + start, size))) {
        return false;
      }
    }
  }

  return true;
}

irs::seek_term_iterator::ptr masking_store_reader::term_reader_t::iterator() const {
  return !documents_ || terms_ .empty()
    ? irs::seek_term_iterator::make<empty_seek_term_iterator>()
    : irs::seek_term_iterator::make<masking_store_term_iterator>(*documents_, meta_->features, terms_);
}

masking_store_reader::masking_store_reader(
    const irs::bitvector& documents,
    fields_t&& fields,
    columns_named_t&& columns_named,
    columns_unnamed_t&& columns_unnamed
): columns_named_(std::move(columns_named)),
   columns_unnamed_(std::move(columns_unnamed)),
   documents_(documents),
   fields_(std::move(fields)) {
  auto& column_by_id = const_cast<column_by_id_t&>(column_by_id_); // initialize map

  for (auto& entry: columns_named_) {
    auto& column = entry.second;

    column_by_id.emplace(column.meta_->id, &column);
    const_cast<named_column_reader_t&>(column).documents_ = &documents_; // update document mask
  }

  for (auto& entry: columns_unnamed_) {
    auto& column = entry.second;

    column_by_id.emplace(entry.first, &column);
    const_cast<column_reader_t&>(column).documents_ = &documents_; // update document mask
  }

  for (auto& entry: fields_) {
    const_cast<term_reader_t&>(entry.second).documents_ = &documents_; // update document mask
  }
}

irs::index_reader::reader_iterator masking_store_reader::begin() const {
  PTR_NAMED(single_reader_iterator_impl, itr, this);

  return index_reader::reader_iterator(itr.release());
}

const irs::column_meta* masking_store_reader::column(
    const irs::string_ref& name
) const {
  auto itr = columns_named_.find(name);

  return itr == columns_named_.end() ? nullptr : itr->second.meta_.get();
}

irs::column_iterator::ptr masking_store_reader::columns() const {
  auto ptr = irs::memory::make_unique<store_column_iterator<columns_named_t>>(columns_named_);

  return irs::memory::make_managed<irs::column_iterator, true>(std::move(ptr));
}

const irs::columnstore_reader::column_reader* masking_store_reader::column_reader(
    irs::field_id field
) const {
  auto itr = column_by_id_.find(field);

  return itr == column_by_id_.end() ? nullptr : itr->second;
}

irs::sub_reader::docs_iterator_t::ptr masking_store_reader::docs_iterator() const {
  auto ptr =
    irs::memory::make_unique<irs::bitset_doc_iterator>(
      *this,
      irs::attribute_store::empty_instance(),
      documents_,
      irs::order::prepared::unordered()
    );

  return ptr;
}

irs::index_reader::reader_iterator masking_store_reader::end() const {
  PTR_NAMED(single_reader_iterator_impl, itr);

  return index_reader::reader_iterator(itr.release());
}

const irs::term_reader* masking_store_reader::field(
    const irs::string_ref& field
) const {
  auto itr = fields_.find(field);

  return itr == fields_.end() ? nullptr : &(itr->second);
}

irs::field_iterator::ptr masking_store_reader::fields() const {
  auto ptr = irs::memory::make_unique<store_field_iterator<fields_t>>(fields_);

  return irs::memory::make_managed<irs::field_iterator, true>(std::move(ptr));
}

NS_END

NS_ROOT

class transaction_store::store_reader_helper {
 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief fill reader state only for the specified documents
  ////////////////////////////////////////////////////////////////////////////////
  template<typename Reader>
  static size_t get_reader_state(
      typename Reader::fields_t& fields,
      typename Reader::columns_named_t& columns_named,
      typename Reader::columns_unnamed_t& columns_unnamed,
      const transaction_store& store,
      const bitvector& documents
  ) {
    fields.clear();
    columns_named.clear();
    columns_unnamed.clear();

    async_utils::read_write_mutex::read_mutex mutex(store.mutex_);
    SCOPED_LOCK(mutex);

    // copy over non-empty columns into an ordered map
    for (auto& columns_entry: store.columns_named_) {
      typename Reader::document_entries_t entries;

      // copy over valid documents
      for (auto& entry: columns_entry.second.entries_) {
        if (entry.buf_ && documents.test(entry.doc_id_)) {
          entries.emplace_back(entry);
        }
      }

      if (entries.empty()) {
        continue; // no docs in column, skip
      }

      std::sort(entries.begin(), entries.end(), DOC_LESS); // sort by doc_id
      columns_named.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(columns_entry.first), // key
        std::forward_as_tuple(columns_entry.second.meta_, std::move(entries)) // value
      );
    }

    // copy over non-empty columns into an ordered map
    for (auto& columns_entry: store.columns_unnamed_) {
      typename Reader::document_entries_t entries;

      // copy over valid documents
      for (auto& entry: columns_entry.second.entries_) {
        if (entry.buf_ && documents.test(entry.doc_id_)) {
          entries.emplace_back(entry);
        }
      }

      if (entries.empty()) {
        continue; // no docs in column, skip
      }

      std::sort(entries.begin(), entries.end(), DOC_LESS); // sort by doc_id
      columns_unnamed.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(columns_entry.first), // key
        std::forward_as_tuple(std::move(entries)) // value
      );
    }

    // copy over non-empty fields into an ordered map
    for (auto& field_entry: store.fields_) {
      bitvector field_docs;
      typename Reader::term_reader_t terms(field_entry.second.meta_);

      // copy over non-empty terms into an ordered map
      for (auto& term_entry: field_entry.second.terms_) {
        typename Reader::document_entries_t postings;

        // copy over valid postings
        for (auto& entry: term_entry.second.entries_) {
          if (entry.buf_ && documents.test(entry.doc_id_)) {
            field_docs.set(entry.doc_id_);
            postings.emplace_back(entry);
          }
        }

        if (postings.empty()) {
          continue; // no docs in term, skip
        }

        std::sort(postings.begin(), postings.end(), DOC_LESS); // sort by doc_id

        auto& term = terms.terms_.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(term_entry.first), // key
          std::forward_as_tuple(term_entry.second.name_, term_entry.second.meta_, std::move(postings)) // value
        ).first->first;

        if (terms.min_term_.null() || terms.min_term_ > term) {
          terms.min_term_ = term; // point at term in reader map
        }

        if (terms.max_term_.null() || terms.max_term_ < term) {
          terms.max_term_ = term; // point at term in reader map
        }
      }

      if (terms.terms_.empty()) {
        continue; // no terms in field, skip
      }

      terms.doc_count_ = field_docs.count();
      map_utils::try_emplace(
        fields,
        field_entry.first, // key
        std::move(terms) // value
      );
    }

    return store.generation_; // obtain store generation while under lock
  }
};

store_reader::store_reader(impl_ptr&& impl) NOEXCEPT
  : impl_(std::move(impl)) {
}

store_reader::store_reader(const store_reader& other) NOEXCEPT {
  *this = other;
}

store_reader& store_reader::operator=(const store_reader& other) NOEXCEPT {
  if (this != &other) {
    // make a copy
    impl_ptr impl = atomic_utils::atomic_load(&other.impl_);

    atomic_utils::atomic_store(&impl_, impl);
  }

  return *this;
}

store_reader store_reader::reopen() const {
  // make a copy
  impl_ptr impl = atomic_utils::atomic_load(&impl_);

  #ifdef IRESEARCH_DEBUG
    auto& reader_impl = dynamic_cast<store_reader_impl&>(*impl);
  #else
    auto& reader_impl = static_cast<store_reader_impl&>(*impl);
  #endif

  {
    async_utils::read_write_mutex::read_mutex mutex(reader_impl.store_.mutex_);
    SCOPED_LOCK(mutex);

    if (reader_impl.store_.generation_ == reader_impl.generation_) {
      return impl; // reuse same reader since there were no changes in store
    }
  }

  return reader_impl.store_.reader(); // store changed, create new reader
}

void store_writer::bstring_output::write_bytes(
    const byte_type* b, size_t size
) {
  oversize(buf_, std::max((pos_ + size) << 1, buf_.size()));
  std::memcpy(&buf_[pos_], b, size);
  pos_ += size;
}

store_writer::store_writer(transaction_store& store) NOEXCEPT
  : next_doc_id_(type_limits<type_t::doc_id_t>::min()), store_(store) {
}

store_writer::~store_writer() {
  async_utils::read_write_mutex::write_mutex mutex(store_.mutex_);
  SCOPED_LOCK(mutex);

  // invalidate in 'store_.valid_doc_ids_' anything in 'used_doc_ids_'
  store_.valid_doc_ids_ -= used_doc_ids_; // free reserved doc_ids
}

bool store_writer::commit() {
  // ensure documents will not be considered for removal if they are being flushed
  SCOPED_LOCK(store_.commit_flush_mutex_); // obtain lock before obtaining reader
  REGISTER_TIMER_DETAILED();

  // ensure doc_ids held by the transaction are always released
  auto cleanup = irs::make_finally([this]()->void {
    async_utils::read_write_mutex::write_mutex mutex(store_.mutex_);
    SCOPED_LOCK(mutex); // reobtain lock, ok since 'store_.commit_flush_mutex_' held

    // invalidate in 'store_.valid_doc_ids_' anything still in 'used_doc_ids_'
    store_.valid_doc_ids_ -= used_doc_ids_; // free reserved doc_ids

    // reset for next run
    modification_queries_.clear();
    next_doc_id_ = type_limits<type_t::doc_id_t>::min();
    used_doc_ids_.clear();
    valid_doc_ids_.clear();
  });
  bitvector invalid_doc_ids;

  // apply removals
  if (!modification_queries_.empty()) {
    masking_store_reader::columns_named_t columns_named;
    masking_store_reader::columns_unnamed_t columns_unnamed;
    bitvector documents = used_doc_ids_; // all documents since some of them might be updates
    masking_store_reader::fields_t fields;

    documents |= store_.visible_docs_;
    transaction_store::store_reader_helper::get_reader_state<masking_store_reader>(
      fields, columns_named, columns_unnamed, store_, documents
    );
    documents.clear();
    documents |= store_.visible_docs_; // all visible doc ids from store + all visible doc ids from writer up to the current update generation

    bitvector processed_documents; // all visible doc ids from writer up to the current update generation
    masking_store_reader reader(
      documents,
      std::move(fields),
      std::move(columns_named),
      std::move(columns_unnamed)
    );

    for (auto& entry: modification_queries_) {
      if (!entry.filter_) {
        continue; // skip null filters since they will not match anything (valid for indexing error during replacement insertion)
      }

      auto filter = entry.filter_->prepare(reader);

      if (!filter) {
        return false; // failed to prepare filter
      }

      auto itr = filter->execute(reader);

      if (!itr) {
        return false; // failed to execute filter
      }

      bool seen = false;

      processed_documents = valid_doc_ids_;
      processed_documents.resize(entry.generation_, true); // compute valid documents up to entry.generation_ (preserve capacity to bypass malloc)
      documents |= processed_documents; // include all visible documents from writer < entry.generation_

      while (itr->next()) {
        auto doc_id = itr->value(); // match was found

        seen = true; // mark query as seen if at least one document matched
        invalid_doc_ids.set(doc_id); // mark doc_id as no longer visible/used
      }

      documents -= invalid_doc_ids; // clear no longer visible doc_ids from next generation pass
      valid_doc_ids_ -= invalid_doc_ids; // clear no longer visible doc_ids from writer

      // for successful updates mark replacement documents as visible
      if (seen) {
        documents |= entry.documents_;
        valid_doc_ids_ |= entry.documents_;
      }
    }
  }

  async_utils::read_write_mutex::write_mutex mutex(store_.mutex_);
  SCOPED_LOCK(mutex); // modifying 'store_.visible_docs_'

  // ensure modification operations below are noexcept
  store_.visible_docs_.reserve(std::max(valid_doc_ids_.size(), invalid_doc_ids.size()));
  used_doc_ids_.reserve(std::max(valid_doc_ids_.size(), invalid_doc_ids.size()));

  ++(store_.generation_); // // mark store state as modified
  store_.visible_docs_ |= valid_doc_ids_; // commit doc_ids
  store_.visible_docs_ -= invalid_doc_ids; // commit removals
  used_doc_ids_ -= valid_doc_ids_; // exclude 'valid' from 'used' (so commited docs would remain valid when transaction is cleaned up)
  used_doc_ids_ |= invalid_doc_ids; // include 'invalid' into 'used' (so removed docs would be invalidated when transaction is cleaned up)

  return true;
}

bool store_writer::index(
    bstring_output& out,
    document::state_t& state,
    const hashed_string_ref& field_name,
    const flags& field_features,
    transaction_store::document_t& doc,
    token_stream& tokens,
    float_t boost
) {
  REGISTER_TIMER_DETAILED();
  auto& attrs = tokens.attributes();
  auto& term = attrs.get<term_attribute>();
  auto& inc = attrs.get<increment>();
  auto& offs = attrs.get<offset>();
  auto& pay = attrs.get<payload>();

  if (!inc) {
    IR_FRMT_ERROR(
      "field '%s' missing required token_stream attribute '%s'",
      field_name.c_str(), increment::type().name().c_str()
    );
    return false;
  }

  if (!term) {
    IR_FRMT_ERROR(
      "field '%s' missing required token_stream attribute '%s'",
      field_name.c_str(), term_attribute::type().name().c_str()
    );
    return false;
  }

  auto field = store_.get_field(field_name, field_features);

  if (!field) {
    IR_FRMT_ERROR(
      "failed to reserve field '%s' for token insertion",
      field_name.c_str()
    );
    return false;
  }

  bool has_freq = field->meta_->features.check<frequency>();
  bool has_offs = has_freq && field->meta_->features.check<offset>() && offs;
  bool has_pay = has_offs && pay;
  bool has_pos = field->meta_->features.check<position>();
  auto& doc_state_offset = map_utils::try_emplace(
    state.offsets_,
    &doc, // key
    irs::integer_traits<size_t>::max() // invalid offset
  ).first->second;

  // write out document stats if they were not written yet
  if (irs::integer_traits<size_t>::const_max == doc_state_offset) {
    static const doc_stats_t initial;

    doc_state_offset = state.out_.file_pointer();
    state.out_.write_bytes(reinterpret_cast<const byte_type*>(&initial), sizeof(decltype(initial)));
  }

  auto& field_state_offset = map_utils::try_emplace(
    state.offsets_,
    &*field, // key
    irs::integer_traits<size_t>::max() // invalid offset
  ).first->second;

  // write out field stats if they were not written yet
  if (irs::integer_traits<size_t>::const_max == field_state_offset) {
    static const field_stats_t initial;

    field_state_offset = state.out_.file_pointer();
    state.out_.write_bytes(reinterpret_cast<const byte_type*>(&initial), sizeof(decltype(initial)));
  }

  while (tokens.next()) {
    size_t term_state_offset; // offset to term state in state buffer

    // insert term if not present and insert document buffer into its postings
    {
      static auto generator = [](
        const hashed_bytes_ref& key,
        const transaction_store::postings_t& value
      ) NOEXCEPT ->hashed_bytes_ref {
        // reuse hash but point ref at value if buffer was allocated succesfully
        return value.name_ ? hashed_bytes_ref(key.hash(), *(value.name_)) : key;
      };

      async_utils::read_write_mutex::write_mutex mutex(store_.mutex_);
      SCOPED_LOCK(mutex);
      auto field_term_itr = map_utils::try_emplace_update_key(
        field->terms_,
        generator,
        make_hashed_ref(term->value(), std::hash<irs::bytes_ref>()), // key
        store_.bstring_pool_, term->value() // value
      );
      auto& field_term = field_term_itr.first->second;

      // new term was inserted which failed to initialize its buffer
      if (field_term_itr.second && !field_term.name_) {
        field->terms_.erase(field_term_itr.first);
        IR_FRMT_ERROR(
          "failed to allocate buffer for term name while indexing new term: %s",
          std::string(ref_cast<char>(term->value()).c_str(), term->value().size()).c_str()
        );

        return false;
      }

      auto& term_state_offset_ref = map_utils::try_emplace(
        state.offsets_,
        &field_term, // key
        irs::integer_traits<size_t>::max() // invalid offset
      ).first->second;

      // if this is the first time this term was seen for this document
      if (irs::integer_traits<size_t>::const_max == term_state_offset_ref) {
        field_term.entries_.emplace_back(doc, out.file_pointer()); // term offset in buffer

        static const term_stats_t initial;

        term_state_offset_ref = state.out_.file_pointer();
        state.out_.write_bytes(reinterpret_cast<const byte_type*>(&initial), sizeof(decltype(initial)));
        ++(reinterpret_cast<field_stats_t*>(state.out_[field_state_offset])->unq_term_count);
      }

      term_state_offset = term_state_offset_ref; // remember offset outside this block
    }

    // get references to meta once state is no longer resized
    auto& document_state = *reinterpret_cast<doc_stats_t*>(state.out_[doc_state_offset]);
    auto& field_state = *reinterpret_cast<field_stats_t*>(state.out_[field_state_offset]);

    field_state.pos += inc->value;

    if (field_state.pos < field_state.pos_last) {
      IR_FRMT_ERROR("invalid position %u < %u", field_state.pos, field_state.pos_last);
      return false; // doc_id will be marked not valid by caller, hence in rollback state
    }

    if (!(inc->value)) {
      ++(field_state.num_overlap); // pos == pos_last
    }

    field_state.pos_last = field_state.pos;

    if (has_offs) {
      const auto offs_start = field_state.offs_start_base + offs->start;
      const auto offs_end = field_state.offs_start_base + offs->end;

      // current term absolute offset start is before the previous term absolute
      // offset start or term-offset-end < term-offset-start
      if (offs_start < field_state.offs_start_term || offs_end < offs_start) {
        IR_FRMT_ERROR("invalid offset start=%u end=%u", offs_start, offs_end);
        return false; // doc_id will be marked not valid by caller, hence in rollback state
      }

      field_state.offs_start_term = offs_start;
    }

    if (0 == ++(document_state.term_count)) {
      IR_FRMT_ERROR("too many token in field, document '" IR_UINT64_T_SPECIFIER "'", doc.doc_id_);
      return false; // doc_id will be marked not valid by caller, hence in rollback state
    }

    auto& term_state = *reinterpret_cast<term_stats_t*>(state.out_[term_state_offset]);
    auto term_start = out.file_pointer(); // remeber start of current term

    //...........................................................................
    // encoded buffer header definition:
    // byte - reserved for internal use
    //
    // encoded block format definition:
    // long   - pointer to the next entry for same doc-term, last == 0 (length of linked list == term frequency in current document)
    // zvint  - position     (present if field.meta.features.check<position>() == true)
    // zvint  - offset start (present if field.meta.features.check<offset>() == true)
    // zvint  - offset end   (present if field.meta.features.check<offset>() == true)
    // byte   - 0 => nullptr payload, !0 => payload follows next
    // string - size + payload (present if previous byte != 0)
    //...........................................................................
    out.write_long(0); // write out placeholder for next entry
    field_state.max_term_freq = std::max(++term_state.term_freq, field_state.max_term_freq);

    if (has_pos) {
      write_zvint(out, field_state.pos); // write out position
    }

    if (has_offs) {
      // add field_state.offs_start_base to shift offsets for repeating terms (no offset overlap)
      write_zvint(out, field_state.offs_start_base + offs->start); // write offset start
      write_zvint(out, field_state.offs_start_base + offs->end); // write offset end
    }

    out.write_byte(has_pay ? 1 : 0); // write has-payload flag

    if (has_pay) {
      write_string(out, pay->value.c_str(), pay->value.size());
    }

    if (term_state.offset) {
      bstring_output prev_out(*out, term_state.offset);

      prev_out.write_long(term_start); // update 'next-pointer' of previous entry
    }

    term_state.offset = term_start; // remeber start of current term
  }

  // get references to meta once state is no longer resized
  auto& document_state = *reinterpret_cast<doc_stats_t*>(state.out_[doc_state_offset]);
  auto& field_state = *reinterpret_cast<field_stats_t*>(state.out_[field_state_offset]);

  field_state.boost *= boost;

  if (offs) {
    field_state.offs_start_base += offs->end; // ending offset of last term
  }

  if (field->meta_->features.check<norm>()) {
    document_state.norm =
      field_state.boost / float_t(std::sqrt(double_t(document_state.term_count)));
  }

  return true;
}

void store_writer::remove(const filter& filter) {
  modification_queries_.emplace_back(filter, next_doc_id_);
}

void store_writer::remove(filter::ptr&& filter) {
  modification_queries_.emplace_back(std::move(filter), next_doc_id_);
}

void store_writer::remove(const std::shared_ptr<filter>& filter) {
  modification_queries_.emplace_back(filter, next_doc_id_);
}

bool store_writer::store(
    bstring_output& out,
    document::state_t& state,
    const hashed_string_ref& column_name,
    transaction_store::document_t& doc,
    size_t buf_offset
) {
  REGISTER_TIMER_DETAILED();
  auto column = store_.get_column(column_name);

  if (!column) {
    IR_FRMT_ERROR(
      "failed to reserve column '%s' for data insertion",
      column_name.c_str()
    );
    return false;
  }

  auto& column_state_offset = map_utils::try_emplace(
    state.offsets_,
    &*column, // key
    irs::integer_traits<size_t>::max() // invalid offset
  ).first->second;

  // if this is the first time this column was seen for this document
  if (irs::integer_traits<size_t>::const_max == column_state_offset) {
    {
      async_utils::read_write_mutex::write_mutex mutex(store_.mutex_);
      SCOPED_LOCK(mutex);
      column->entries_.emplace_back(doc, out.file_pointer()); // column offset in buffer
    }

    static const column_stats_t initial;

    column_state_offset = state.out_.file_pointer(); // same as in 'entries_'
    state.out_.write_bytes(reinterpret_cast<const byte_type*>(&initial), sizeof(decltype(initial)));
  }

  auto& column_state = *reinterpret_cast<column_stats_t*>(state.out_[column_state_offset]);
  auto column_start = out.file_pointer(); // remeber start of current column

  //...........................................................................
  // encoded buffer header definition:
  // byte - reserved for internal use
  //
  // encoded block format definition:
  // bytes  - user data
  // long   - pointer to the next entry (points at its 'next-pointer') for same doc-term, last == 0 (length of linked list == column frequency in current document)
  // vlong  - delta length of user data up to 'next-pointer'
  //...........................................................................
  out.write_long(0); // write out placeholder for next entry
  out.write_vlong(column_start - buf_offset); // write out delta to start of data

  if (column_state.offset) {
    bstring_output prev_out(*out, column_state.offset);

    prev_out.write_long(column_start); // update 'next-pointer' of previous entry
  }

  column_state.offset = column_start; // remeber start of current column

  return true;
}

/*static*/ transaction_store::bstring_builder::ptr transaction_store::bstring_builder::make() {
  return irs::memory::make_unique<bstring>(DEFAULT_BUFFER_SIZE, '\0');
}

/*static*/ transaction_store::column_meta_builder::ptr transaction_store::column_meta_builder::make() {
  return irs::memory::make_unique<column_meta>();
}

/*static*/ transaction_store::field_meta_builder::ptr transaction_store::field_meta_builder::make() {
  return irs::memory::make_unique<field_meta>();
}

transaction_store::transaction_store(size_t pool_size /*= DEFAULT_POOL_SIZE*/)
  : bstring_pool_(pool_size),
    column_meta_pool_(pool_size),
    field_meta_pool_(pool_size),
    generation_(0),
    used_doc_ids_(type_limits<type_t::doc_id_t>::invalid() + 1),
    visible_docs_(type_limits<type_t::doc_id_t>::invalid() + 1) { // same size as used_doc_ids_
  used_doc_ids_.set(type_limits<type_t::doc_id_t>::invalid()); // mark as always in-use
}

bool transaction_store::flush(index_writer& writer) {
  store_reader_impl::columns_named_t columns_named;
  store_reader_impl::columns_unnamed_t columns_unnamed;
  store_reader_impl::fields_t fields;

  // ensure flush is not called concurrently and partial commit/flush are not visible
  SCOPED_LOCK(commit_flush_mutex_); // obtain lock before obtaining reader
  REGISTER_TIMER_DETAILED();

  transaction_store::store_reader_helper::get_reader_state<store_reader_impl>(
    fields, columns_named, columns_unnamed, *this, visible_docs_
  );

  store_reader_impl reader(
    *this,
    irs::bitvector(visible_docs_),
    std::move(fields),
    std::move(columns_named),
    std::move(columns_unnamed),
    generation_
  );

  if (!writer.import(reader)) {
    return false;
  }

  async_utils::read_write_mutex::write_mutex mutex(mutex_);
  SCOPED_LOCK(mutex);

  ++generation_; // mark state as modified
  used_doc_ids_ -= visible_docs_; // remove flushed ids from 'used'
  valid_doc_ids_ -= visible_docs_; // remove flushed ids from 'valid'
  visible_docs_.clear(); // remove flushed ids from 'visible'

  //...........................................................................
  // remove no longer used records from internal maps
  //...........................................................................

  // remove unused records from named user columns
  for (auto itr = columns_named_.begin(), end = columns_named_.end(); itr != end;) {
    auto& column = itr->second;
    size_t last = column.entries_.size() - 1;

    for (auto i = column.entries_.size(); i;) {
      auto& record = column.entries_[--i];

      if (used_doc_ids_.test(record.doc_id_)) {
        continue; // record still in use
      }

      record = std::move(column.entries_[last--]); // replace unused record
      column.entries_.pop_back(); // remove moved record
    }

    if (!column.entries_.empty() || column.refs_) {
      ++itr;
      continue; // column still in use
    }

    used_column_ids_.unset(column.meta_->id); // release column id
    itr = columns_named_.erase(itr);
  }

  // remove unused records from system columns
  for (auto itr = columns_unnamed_.begin(), end = columns_unnamed_.end(); itr != end;) {
    auto& column = itr->second;
    auto id = itr->first;
    size_t last = column.entries_.size() - 1;

    for (auto i = column.entries_.size(); i;) {
      auto& record = column.entries_[--i];

      if (used_doc_ids_.test(record.doc_id_)) {
        continue; // record still in use
      }

      record = std::move(column.entries_[last--]); // replace unused record
      column.entries_.pop_back(); // remove moved record
    }

    if (!column.entries_.empty() || column.refs_) {
      ++itr;
      continue; // column still in use
    }

    used_column_ids_.unset(id); // release column id
    itr = columns_unnamed_.erase(itr);
  }

  // remove unused records from fields
  for (auto itr = fields_.begin(), end = fields_.end(); itr != end;) {
    auto& field = itr->second;

    for (auto term_itr = field.terms_.begin(), terms_end = field.terms_.end();
         term_itr != terms_end;
        ) {
      auto& term = term_itr->second;
      size_t last = term.entries_.size() - 1;

      for (auto i = term.entries_.size(); i;) {
        auto& record = term.entries_[--i];

        if (used_doc_ids_.test(record.doc_id_)) {
          continue; // record still in use
        }

        record = std::move(term.entries_[last--]); // replace unused record
        term.entries_.pop_back(); // remove moved record
      }

      if (!term.entries_.empty()) {
        ++term_itr;
        continue; // term still in use
      }

      term_itr = field.terms_.erase(term_itr);
    }

    if (!field.terms_.empty() || field.refs_) {
      ++itr;
      continue; // field still in use
    }

    itr = fields_.erase(itr);
  }

  return true;
}

transaction_store::column_ref_t transaction_store::get_column(
    const hashed_string_ref& name
) {
  REGISTER_TIMER_DETAILED();
  static auto generator = [](
    const hashed_string_ref& key,
    const transaction_store::column_named_t& value
  ) NOEXCEPT ->hashed_string_ref {
    // reuse hash but point ref at value if buffer was allocated succesfully
    return value.meta_ ? hashed_string_ref(key.hash(), value.meta_->name) : key;
  };

  async_utils::read_write_mutex::write_mutex mutex(mutex_);
  SCOPED_LOCK(mutex);

  auto itr = map_utils::try_emplace_update_key(
    columns_named_,
    generator,
    name, // key
    column_meta_pool_, name, type_limits<type_t::field_id_t>::invalid() // value
  );
  auto& column = itr.first->second;

  // new column was inserted, assign column id
  if (itr.second) {
    // new column was inserted which failed to initialize its meta
    if (!column.meta_) {
      columns_named_.erase(itr.first);
      IR_FRMT_ERROR(
        "failed to allocate buffer for column meta while indexing new column: %s",
        std::string(name.c_str(), name.size()).c_str()
      );
    }

    column.meta_->id = get_column_id();

    if (!type_limits<type_t::field_id_t>::valid(column.meta_->id)) {
      columns_named_.erase(itr.first);

      return column_ref_t();
    }
  }

  // increment ref counter under write lock to coordinate with flush(...)
  return column_ref_t(column);
}

field_id transaction_store::get_column_id() {
  REGISTER_TIMER_DETAILED();
  field_id start = 0;
  async_utils::read_write_mutex::write_mutex mutex(mutex_);
  SCOPED_LOCK(mutex);

  while (type_limits<type_t::field_id_t>::valid(start)) {
    if (!used_column_ids_.test(start)) {
      used_column_ids_.set(start);

      return start;
    }

    // optimization to skip over words with all bits set
    start = *(used_column_ids_.begin() + bitset::word(start)) == bitset::word_t(-1)
      ? bitset::bit_offset(bitset::word(start) + 1) // set to first bit of next word
      : (start + 1)
      ;
  }

  return type_limits<type_t::field_id_t>::invalid();
}

doc_id_t transaction_store::get_doc_id(doc_id_t start) {
  REGISTER_TIMER_DETAILED();
  if (start == type_limits<type_t::doc_id_t>::eof() ||
      start == type_limits<type_t::doc_id_t>::invalid()) {
    return type_limits<type_t::doc_id_t>::invalid();
  }

  async_utils::read_write_mutex::write_mutex mutex(mutex_);
  SCOPED_LOCK(mutex);

  while (!type_limits<type_t::doc_id_t>::eof(start)) {
    if (!used_doc_ids_.test(start)) {
      visible_docs_.reserve(start); // ensure all allocation happends here
      used_doc_ids_.set(start);
      valid_doc_ids_.set(start);

      return start;
    }

    // optimization to skip over words with all bits set
    start = *(used_doc_ids_.begin() + bitset::word(start)) == bitset::word_t(-1)
      ? bitset::bit_offset(bitset::word(start) + 1) // set to first bit of next word
      : (start + 1)
      ;
  }

  return type_limits<type_t::doc_id_t>::invalid();
}

transaction_store::field_ref_t transaction_store::get_field(
    const hashed_string_ref& name,
    const flags& features
) {
  REGISTER_TIMER_DETAILED();
  static auto generator = [](
    const hashed_string_ref& key,
    const transaction_store::terms_t& value
  ) NOEXCEPT ->hashed_string_ref {
    // reuse hash but point ref at value if buffer was allocated succesfully
    return value.meta_ ? hashed_string_ref(key.hash(), value.meta_->name) : key;
  };

  async_utils::read_write_mutex::write_mutex mutex(mutex_);
  SCOPED_LOCK(mutex);
  auto itr = map_utils::try_emplace_update_key(
    fields_,
    generator,
    name, // key
    field_meta_pool_, name, features // value
  );
  auto& field = itr.first->second;

  // new field was inserted
  if (itr.second) {
    // new field was inserted which failed to initialize its meta
    if (!field.meta_) {
      fields_.erase(itr.first);
      IR_FRMT_ERROR(
        "failed to allocate buffer for field meta while indexing new field: %s",
        std::string(name.c_str(), name.size()).c_str()
      );

      return field_ref_t();
    }

    field.meta_->features |= features;

    // if 'norm' required then create the corresponding 'norm' column
    if (field.meta_->features.check<irs::norm>()) {
      auto norm_col_id = get_column_id();

      if (!type_limits<type_t::field_id_t>::valid(norm_col_id)) {
        fields_.erase(itr.first);

        return field_ref_t();
      }

      field.norm_col_ref_ = ref_t<column_t>(columns_unnamed_[norm_col_id]);
    }

    // increment ref counter under write lock to coordinate with flush(...)
    return field_ref_t(field);
  }

  // increment ref counter under write lock to coordinate with flush(...)
  return features.is_subset_of(field.meta_->features)
    ? field_ref_t(field)
    : field_ref_t() // new field features are not a subset of existing field features
    ;
}

store_reader transaction_store::reader() const {
  REGISTER_TIMER_DETAILED();
  store_reader_impl::columns_named_t columns_named;
  store_reader_impl::columns_unnamed_t columns_unnamed;
  bitvector documents;
  store_reader_impl::fields_t fields;
  size_t generation;

  {
    async_utils::read_write_mutex::read_mutex mutex(mutex_);
    SCOPED_LOCK(mutex);
    documents = visible_docs_;
    generation= transaction_store::store_reader_helper::get_reader_state<store_reader_impl>(
       fields, columns_named, columns_unnamed, *this, documents
     );
  }

  documents.shrink_to_fit();

  PTR_NAMED(
    store_reader_impl,
    reader,
    *this,
    std::move(documents),
    std::move(fields),
    std::move(columns_named),
    std::move(columns_unnamed),
    generation
  );

  return reader;
}

NS_END // ROOT

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------