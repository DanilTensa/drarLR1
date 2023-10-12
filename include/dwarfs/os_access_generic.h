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
#include <filesystem>
#include <memory>
#include <string>

#include "dwarfs/os_access.h"

namespace dwarfs {

class mmif;

class os_access_generic : public os_access {
 public:
  std::shared_ptr<dir_reader>
  opendir(std::filesystem::path const& path) const override;
  file_stat symlink_info(std::filesystem::path const& path) const override;
  std::filesystem::path
  read_symlink(std::filesystem::path const& path) const override;
  std::shared_ptr<mmif>
  map_file(std::filesystem::path const& path, size_t size) const override;
  int access(std::filesystem::path const& path, int mode) const override;
};
} // namespace dwarfs
