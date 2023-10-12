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

#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>

#include <folly/Expected.h>

#include "dwarfs/block_range.h"
#include "dwarfs/metadata_types.h"
#include "dwarfs/types.h"

namespace dwarfs {

struct cache_tidy_config;
class block_cache;
class logger;
struct iovec_read_buf;
class performance_monitor;

class inode_reader_v2 {
 public:
  inode_reader_v2() = default;

  inode_reader_v2(logger& lgr, block_cache&& bc,
                  std::shared_ptr<performance_monitor const> perfmon);

  inode_reader_v2& operator=(inode_reader_v2&&) = default;

  ssize_t read(char* buf, uint32_t inode, size_t size, file_off_t offset,
               chunk_range chunks) const {
    return impl_->read(buf, inode, size, offset, chunks);
  }

  ssize_t readv(iovec_read_buf& buf, uint32_t inode, size_t size,
                file_off_t offset, chunk_range chunks) const {
    return impl_->readv(buf, inode, size, offset, chunks);
  }

  folly::Expected<std::vector<std::future<block_range>>, int>
  readv(uint32_t inode, size_t size, file_off_t offset,
        chunk_range chunks) const {
    return impl_->readv(inode, size, offset, chunks);
  }

  void
  dump(std::ostream& os, const std::string& indent, chunk_range chunks) const {
    impl_->dump(os, indent, chunks);
  }

  void set_num_workers(size_t num) { impl_->set_num_workers(num); }

  void set_cache_tidy_config(cache_tidy_config const& cfg) {
    impl_->set_cache_tidy_config(cfg);
  }

  size_t num_blocks() const { return impl_->num_blocks(); }

  class impl {
   public:
    virtual ~impl() = default;

    virtual ssize_t read(char* buf, uint32_t inode, size_t size,
                         file_off_t offset, chunk_range chunks) const = 0;
    virtual ssize_t readv(iovec_read_buf& buf, uint32_t inode, size_t size,
                          file_off_t offset, chunk_range chunks) const = 0;
    virtual folly::Expected<std::vector<std::future<block_range>>, int>
    readv(uint32_t inode, size_t size, file_off_t offset,
          chunk_range chunks) const = 0;
    virtual void dump(std::ostream& os, const std::string& indent,
                      chunk_range chunks) const = 0;
    virtual void set_num_workers(size_t num) = 0;
    virtual void set_cache_tidy_config(cache_tidy_config const& cfg) = 0;
    virtual size_t num_blocks() const = 0;
  };

 private:
  std::unique_ptr<impl> impl_;
};
} // namespace dwarfs
