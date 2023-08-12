/* vim:set ts=2 sw=2 sts=2 et: */
/**
 * \author     Marcus Holland-Moritz (github@mhxnet.de)
 * \copyright  Copyright (c) Marcus Holland-Moritz
 *
 * This file is part of dwarfs.
 *
 * dwarfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dwarfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dwarfs.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <numeric>
#include <ostream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include <folly/Demangle.h>
#include <folly/small_vector.h>
#include <folly/sorted_vector_types.h>

#include "dwarfs/categorizer.h"
#include "dwarfs/compiler.h"
#include "dwarfs/entry.h"
#include "dwarfs/error.h"
#include "dwarfs/inode_manager.h"
#include "dwarfs/inode_ordering.h"
#include "dwarfs/logger.h"
#include "dwarfs/mmif.h"
#include "dwarfs/nilsimsa.h"
#include "dwarfs/options.h"
#include "dwarfs/os_access.h"
#include "dwarfs/overloaded.h"
#include "dwarfs/progress.h"
#include "dwarfs/promise_receiver.h"
#include "dwarfs/script.h"
#include "dwarfs/similarity.h"
#include "dwarfs/similarity_ordering.h"
#include "dwarfs/worker_group.h"

#include "dwarfs/gen-cpp2/metadata_types.h"

namespace dwarfs {

namespace {

class inode_ : public inode {
 public:
  using chunk_type = thrift::metadata::chunk;

  inode_() {
    std::fill(nilsimsa_similarity_hash_.begin(),
              nilsimsa_similarity_hash_.end(), 0);
  }

  void set_num(uint32_t num) override {
    DWARFS_CHECK((flags_ & kNumIsValid) == 0,
                 "attempt to set inode number multiple times");
    num_ = num;
    flags_ |= kNumIsValid;
  }

  uint32_t num() const override {
    DWARFS_CHECK((flags_ & kNumIsValid) != 0, "inode number is not set");
    return num_;
  }

  bool has_category(fragment_category cat) const override {
    DWARFS_CHECK(!fragments_.empty(),
                 "has_category() called with no fragments");
    if (fragments_.size() == 1) {
      return fragments_.get_single_category() == cat;
    }
    auto& m = std::get<similarity_map_type>(similarity_);
    return m.find(cat) != m.end();
  }

  uint32_t similarity_hash() const override {
    if (files_.empty()) {
      DWARFS_THROW(runtime_error, "inode has no file (similarity)");
    }
    // TODO
    return similarity_hash_;
  }

  nilsimsa::hash_type const& nilsimsa_similarity_hash() const override {
    if (files_.empty()) {
      DWARFS_THROW(runtime_error, "inode has no file (nilsimsa)");
    }
    // TODO
    return nilsimsa_similarity_hash_;
  }

  uint32_t similarity_hash(fragment_category cat) const override {
    return find_similarity<uint32_t>(cat);
  }

  nilsimsa::hash_type const&
  nilsimsa_similarity_hash(fragment_category cat) const override {
    return find_similarity<nilsimsa::hash_type>(cat);
  }

  void set_files(files_vector&& fv) override {
    if (!files_.empty()) {
      DWARFS_THROW(runtime_error, "files already set for inode");
    }

    files_ = std::move(fv);
  }

  void scan(mmif* mm, inode_options const& opts) override {
    categorizer_job catjob;

    // No job if categorizers are disabled
    if (opts.categorizer_mgr) {
      catjob =
          opts.categorizer_mgr->job(mm ? mm->path().string() : "<no-file>");
    }

    /// TODO: remove comments or move elsewhere
    ///
    /// 1. Run random access categorizers
    /// 2. If we *have* a best category already (need a call for that),
    ///    we can immediately compute similarity hashes for all fragments
    ///    (or not, if the category is configured not to use similarity)
    /// 3. If we *don't* have a best category yet, we can run similarity
    ///    hashing while running the sequential categorizer(s).
    /// 4. If we end up with multiple fragments after all, we might have
    ///    to re-run similarity hashing. This means we can also drop the
    ///    multi-fragment sequential categorizer check, as we can just
    ///    as well support that case.
    ///

    // If we don't have a mapping, we can't scan anything
    if (mm) {
      if (catjob) {
        // First, run random access categorizers. If we get a result here,
        // it's very likely going to be the best result.
        catjob.set_total_size(mm->size());
        catjob.categorize_random_access(mm->span());

        if (catjob.best_result_found()) {
          // This means the job won't be running any sequential categorizers
          // as the outcome cannot possibly be any better. As a consequence,
          // we can already fetch the result here and scan the fragments
          // instead of the whole file.

          fragments_ = catjob.result();

          if (fragments_.size() > 1) {
            scan_fragments(mm, opts);
          } else {
            scan_full(mm, opts);
          }
        }
      }

      if (fragments_.empty()) {
        // If we get here, we haven't scanned anything yet, and we don't know
        // if the file will be fragmented or not.

        scan_full(mm, opts);

        if (catjob) {
          fragments_ = catjob.result();

          if (fragments_.size() > 1) {
            // This is the unfortunate case where we have to scan the
            // individual fragments after having already done a full scan.
            scan_fragments(mm, opts);
          }
        }
      }
    }

    // Add a fragment if nothing has been added so far. We need a single
    // fragment to store the inode's chunks. This won't use up any resources
    // as a single fragment is stored inline.
    if (fragments_.empty()) {
      fragments_.emplace_back(categorizer_manager::default_category(),
                              mm ? mm->size() : 0);
    }
  }

  void add_chunk(size_t block, size_t offset, size_t size) override {
    chunk_type c;
    c.block() = block;
    c.offset() = offset;
    c.size() = size;
    chunks_.push_back(c);
  }

  size_t size() const override { return any()->size(); }

  files_vector const& files() const override { return files_; }

  file const* any() const override {
    if (files_.empty()) {
      DWARFS_THROW(runtime_error, "inode has no file (any)");
    }
    return files_.front();
  }

  void append_chunks_to(std::vector<chunk_type>& vec) const override {
    vec.insert(vec.end(), chunks_.begin(), chunks_.end());
  }

  inode_fragments const& fragments() const override { return fragments_; }

  void dump(std::ostream& os, inode_options const& options) const override {
    auto dump_category = [&os, &options](fragment_category const& cat) {
      if (options.categorizer_mgr) {
        os << "[" << options.categorizer_mgr->category_name(cat.value());
        if (cat.has_subcategory()) {
          os << "/" << cat.subcategory();
        }
        os << "] ";
      }
    };

    os << "inode " << num() << " (" << any()->size() << "):\n";
    os << "  files:\n";

    for (auto const& f : files_) {
      os << "    " << f->path_as_string() << "\n";
    }

    os << "  fragments:\n";

    for (auto const& f : fragments_.span()) {
      os << "    ";
      dump_category(f.category());
      os << "(" << f.size() << " bytes)\n";
      for (auto const& c : f.chunks()) {
        os << "      (" << c.get_block() << ", " << c.get_offset() << ", "
           << c.get_size() << ")\n";
      }
    }

    os << "  similarity: ";

    auto basic_hash_visitor = [&os](uint32_t sh) {
      os << fmt::format("basic ({0:08x})\n", sh);
    };

    auto nilsimsa_hash_visitor = [&os](nilsimsa::hash_type const& nh) {
      os << fmt::format("nilsimsa ({0:016x}{1:016x}{2:016x}{3:016x})\n", nh[0],
                        nh[1], nh[2], nh[3]);
    };

    auto similarity_map_visitor = [&](similarity_map_type const& map) {
      os << "map\n";
      for (auto const& [cat, val] : map) {
        os << "    ";
        dump_category(cat);
        std::visit(
            overloaded{
                basic_hash_visitor,
                nilsimsa_hash_visitor,
            },
            val);
      }
    };

    std::visit(
        overloaded{
            [&os](std::monostate const&) { os << "none\n"; },
            basic_hash_visitor,
            nilsimsa_hash_visitor,
            similarity_map_visitor,
        },
        similarity_);
  }

 private:
  template <typename T>
  T const& find_similarity(fragment_category cat) const {
    if (fragments_.empty()) [[unlikely]] {
      DWARFS_THROW(runtime_error, fmt::format("inode has no fragments ({})",
                                              folly::demangle(typeid(T))));
    }
    if (fragments_.size() == 1) {
      if (fragments_.get_single_category() != cat) [[unlikely]] {
        DWARFS_THROW(runtime_error, fmt::format("category mismatch ({})",
                                                folly::demangle(typeid(T))));
      }
      return std::get<T>(similarity_);
    }
    auto& m = std::get<similarity_map_type>(similarity_);
    if (auto it = m.find(cat); it != m.end()) {
      return std::get<T>(it->second);
    }
    DWARFS_THROW(runtime_error, fmt::format("category not found ({})",
                                            folly::demangle(typeid(T))));
  }

  template <typename T>
  void scan_range(mmif* mm, size_t offset, size_t size, T&& scanner) {
    static constexpr size_t const chunk_size = 32 << 20;

    while (size >= chunk_size) {
      scanner(mm->span(offset, chunk_size));
      mm->release_until(offset);
      offset += chunk_size;
      size -= chunk_size;
    }

    scanner(mm->span(offset, size));
  }

  void scan_fragments(mmif* mm, inode_options const& opts) {
    assert(mm);
    assert(fragments_.size() > 1);

    std::unordered_map<fragment_category, similarity> sc;
    std::unordered_map<fragment_category, nilsimsa> nc;

    for (auto const& f : fragments_.span()) {
      switch (opts.fragment_order.get(f.category()).mode) {
      case file_order_mode::NONE:
      case file_order_mode::PATH:
      case file_order_mode::SCRIPT:
        break;
      case file_order_mode::SIMILARITY:
        sc.try_emplace(f.category());
        break;
      case file_order_mode::NILSIMSA:
        nc.try_emplace(f.category());
        break;
      }
    }

    if (sc.empty() && nc.empty()) {
      return;
    }

    file_off_t pos = 0;

    for (auto const& f : fragments_.span()) {
      auto const size = f.length();

      if (auto i = sc.find(f.category()); i != sc.end()) {
        scan_range(mm, pos, size, i->second);
      } else if (auto i = nc.find(f.category()); i != nc.end()) {
        scan_range(mm, pos, size, i->second);
      }

      pos += size;
    }

    similarity_map_type tmp_map;

    for (auto const& [cat, hasher] : sc) {
      tmp_map.emplace(cat, hasher.finalize());
    }

    for (auto const& [cat, hasher] : nc) {
      // TODO: can we finalize in-place?
      nilsimsa::hash_type hash;
      hasher.finalize(hash);
      tmp_map.emplace(cat, hash);
    }

    similarity_.emplace<similarity_map_type>(std::move(tmp_map));
  }

  void scan_full(mmif* mm, inode_options const& opts) {
    assert(mm);
    assert(fragments_.size() <= 1);

    auto order_mode =
        fragments_.empty()
            ? opts.fragment_order.get().mode
            : opts.fragment_order.get(fragments_.get_single_category()).mode;

    switch (order_mode) {
    case file_order_mode::NONE:
    case file_order_mode::PATH:
    case file_order_mode::SCRIPT:
      break;

    case file_order_mode::SIMILARITY: {
      similarity sc;
      scan_range(mm, 0, mm->size(), sc);
      similarity_hash_ = sc.finalize(); // TODO
      similarity_.emplace<uint32_t>(sc.finalize());
    } break;

    case file_order_mode::NILSIMSA: {
      nilsimsa nc;
      scan_range(mm, 0, mm->size(), nc);
      // TODO: can we finalize in-place?
      nilsimsa::hash_type hash;
      nc.finalize(hash);
      nilsimsa_similarity_hash_ = hash; // TODO
      similarity_.emplace<nilsimsa::hash_type>(hash);
    } break;
    }
  }

  using similarity_map_type =
      folly::sorted_vector_map<fragment_category,
                               std::variant<nilsimsa::hash_type, uint32_t>>;

  static constexpr uint32_t const kNumIsValid{UINT32_C(1) << 0};

  uint32_t flags_{0};
  uint32_t num_;
  inode_fragments fragments_;
  files_vector files_;

  std::variant<
      // in case of no hashes at all
      std::monostate,

      // in case of only a single fragment
      nilsimsa::hash_type, // 32 bytes
      uint32_t,            //  4 bytes

      // in case of multiple fragments
      similarity_map_type // 24 bytes
      >
      similarity_;

  // OLDE:
  uint32_t similarity_hash_{0};    // TODO: remove (move to similarity_)
  std::vector<chunk_type> chunks_; // TODO: remove (part of fragments_ now)
  nilsimsa::hash_type
      nilsimsa_similarity_hash_; // TODO: remove (move to similarity_)
};

} // namespace

template <typename LoggerPolicy>
class inode_manager_ final : public inode_manager::impl {
 public:
  inode_manager_(logger& lgr, progress& prog, inode_options const& opts)
      : LOG_PROXY_INIT(lgr)
      , prog_(prog)
      , opts_{opts}
      , inodes_need_scanning_{inodes_need_scanning(opts_)} {}

  std::shared_ptr<inode> create_inode() override {
    auto ino = std::make_shared<inode_>();
    inodes_.push_back(ino);
    return ino;
  }

  size_t count() const override { return inodes_.size(); }

  void order_inodes(worker_group& wg, std::shared_ptr<script> scr,
                    inode_manager::order_cb const& fn) override;

  void for_each_inode_in_order(
      std::function<void(std::shared_ptr<inode> const&)> const& fn)
      const override {
    auto span = sortable_span();
    span.all();
    inode_ordering(LOG_GET_LOGGER, prog_).by_inode_number(span);
    for (auto const& i : span) {
      fn(i);
    }
  }

  std::vector<std::pair<fragment_category::value_type, size_t>>
  fragment_category_counts() const override {
    std::unordered_map<fragment_category::value_type, size_t> tmp;

    for (auto const& i : inodes_) {
      if (auto const& fragments = i->fragments(); !fragments.empty()) {
        for (auto const& frag : fragments) {
          ++tmp[frag.category().value()];
        }
      }
    }

    std::vector<std::pair<fragment_category::value_type, size_t>> rv;

    for (auto const& [k, v] : tmp) {
      rv.emplace_back(k, v);
    }

    std::sort(rv.begin(), rv.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    return rv;
  }

  std::vector<fragment_category> inode_categories() const override {
    std::unordered_set<fragment_category> tmp;

    for (auto const& i : inodes_) {
      if (auto const& fragments = i->fragments(); !fragments.empty()) {
        for (auto const& frag : fragments) {
          tmp.emplace(frag.category());
        }
      }
    }

    std::vector<fragment_category> rv(tmp.begin(), tmp.end());

    std::sort(rv.begin(), rv.end());

    return rv;
  }

  void
  scan_background(worker_group& wg, os_access& os, std::shared_ptr<inode> ino,
                  file const* p) const override;

  void dump(std::ostream& os) const override;

  sortable_inode_span sortable_span() const override {
    return sortable_inode_span(inodes_);
  }

 private:
  static bool inodes_need_scanning(inode_options const& opts) {
    if (opts.categorizer_mgr) {
      return true;
    }

    return opts.fragment_order.any_is([](auto const& order) {
      return order.mode == file_order_mode::SIMILARITY ||
             order.mode == file_order_mode::NILSIMSA;
    });
  }

  void order_inodes_by_path() {
    auto span = sortable_span();
    span.all();
    inode_ordering(LOG_GET_LOGGER, prog_).by_path(span);

    std::vector<std::shared_ptr<inode>> tmp(span.begin(), span.end());
    inodes_.swap(tmp);
  }

  void order_inodes_by_similarity() {
    auto span = sortable_span();
    span.all();
    inode_ordering(LOG_GET_LOGGER, prog_).by_similarity(span);

    std::vector<std::shared_ptr<inode>> tmp(span.begin(), span.end());
    inodes_.swap(tmp);
  }

  void presort_index(std::vector<std::shared_ptr<inode>>& inodes,
                     std::vector<uint32_t>& index);

  void order_inodes_by_nilsimsa(worker_group& wg);

  LOG_PROXY_DECL(LoggerPolicy);
  std::vector<std::shared_ptr<inode>> inodes_;
  progress& prog_;
  inode_options opts_;
  bool const inodes_need_scanning_;
};

template <typename LoggerPolicy>
void inode_manager_<LoggerPolicy>::scan_background(worker_group& wg,
                                                   os_access& os,
                                                   std::shared_ptr<inode> ino,
                                                   file const* p) const {
  // TODO: I think the size check makes everything more complex.
  //       If we don't check the size, we get the code to run
  //       that ensures `fragments_` is updated. Also, there
  //       should only ever be one empty inode, so the check
  //       doesn't actually make much of a difference.
  if (inodes_need_scanning_ /* && p->size() > 0 */) {
    wg.add_job([this, &os, p, ino = std::move(ino)] {
      auto const size = p->size();
      std::shared_ptr<mmif> mm;
      if (size > 0) {
        mm = os.map_file(p->fs_path(), size);
      }
      ino->scan(mm.get(), opts_);
      ++prog_.similarity_scans; // TODO: we probably don't want this here
      prog_.similarity_bytes += size;
      ++prog_.inodes_scanned;
      ++prog_.files_scanned;
    });
  } else {
    ++prog_.inodes_scanned;
    ++prog_.files_scanned;
  }
}

template <typename LoggerPolicy>
void inode_manager_<LoggerPolicy>::order_inodes(
    worker_group& wg, std::shared_ptr<script> scr,
    inode_manager::order_cb const& fn) {
  // TODO: only use an index, never actually reorder inodes

  // TODO:
  switch (opts_.fragment_order.get().mode) {
  case file_order_mode::NONE:
    LOG_INFO << "keeping inode order";
    break;

  case file_order_mode::PATH: {
    LOG_INFO << "ordering " << count() << " inodes by path name...";
    auto ti = LOG_CPU_TIMED_INFO;
    order_inodes_by_path();
    ti << count() << " inodes ordered";
    break;
  }

  case file_order_mode::SCRIPT: {
    if (!scr->has_order()) {
      DWARFS_THROW(runtime_error, "script cannot order inodes");
    }
    LOG_INFO << "ordering " << count() << " inodes using script...";
    auto ti = LOG_CPU_TIMED_INFO;
    scr->order(inodes_);
    ti << count() << " inodes ordered";
    break;
  }

  case file_order_mode::SIMILARITY: {
    LOG_INFO << "ordering " << count() << " inodes by similarity...";
    auto ti = LOG_CPU_TIMED_INFO;
    order_inodes_by_similarity();
    ti << count() << " inodes ordered";
    break;
  }

  case file_order_mode::NILSIMSA: {
    LOG_INFO << "ordering " << count()
             << " inodes using new nilsimsa similarity...";
    auto ti = LOG_CPU_TIMED_INFO;
    order_inodes_by_nilsimsa(wg);
    ti << count() << " inodes ordered";
    break;
  }
  }

  LOG_INFO << "assigning file inodes...";
  for (const auto& ino : inodes_) {
    fn(ino);
  }
}

template <typename LoggerPolicy>
void inode_manager_<LoggerPolicy>::presort_index(
    std::vector<std::shared_ptr<inode>>& inodes, std::vector<uint32_t>& index) {
  auto ti = LOG_TIMED_INFO;
  size_t num_name = 0;
  size_t num_path = 0;

  std::sort(index.begin(), index.end(), [&](auto a, auto b) {
    auto const& ia = *inodes[a];
    auto const& ib = *inodes[b];
    auto sa = ia.size();
    auto sb = ib.size();

    if (sa < sb) {
      return true;
    } else if (sa > sb) {
      return false;
    }

    ++num_name;

    auto fa = ia.any();
    auto fb = ib.any();
    auto& na = fa->name();
    auto& nb = fb->name();

    if (na > nb) {
      return true;
    } else if (na < nb) {
      return false;
    }

    ++num_path;

    return !fa->less_revpath(*fb);
  });

  ti << "pre-sorted index (" << num_name << " name, " << num_path
     << " path lookups)";
}

template <typename LoggerPolicy>
void inode_manager_<LoggerPolicy>::order_inodes_by_nilsimsa(worker_group& wg) {
  auto const& file_order = opts_.fragment_order.get(); // TODO
  similarity_ordering_options opts;
  opts.max_children = file_order.nilsimsa_max_children;
  opts.max_cluster_size = file_order.nilsimsa_max_cluster_size;

  auto span = sortable_span();
  span.all();

  inode_ordering(LOG_GET_LOGGER, prog_).by_nilsimsa(wg, opts, span);

  std::vector<std::shared_ptr<inode>> tmp(span.begin(), span.end());
  inodes_.swap(tmp);
}

template <typename LoggerPolicy>
void inode_manager_<LoggerPolicy>::dump(std::ostream& os) const {
  for_each_inode_in_order(
      [this, &os](auto const& ino) { ino->dump(os, opts_); });
}

inode_manager::inode_manager(logger& lgr, progress& prog,
                             inode_options const& opts)
    : impl_(make_unique_logging_object<impl, inode_manager_, logger_policies>(
          lgr, prog, opts)) {}

} // namespace dwarfs
