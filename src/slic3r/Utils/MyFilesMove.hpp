#pragma once

#include <boost/filesystem/path.hpp>
#include <string>

namespace Slic3r {

// File moves of the My Files library. Kept free of the GUI so they can be tested, since a mistake here loses the
// user's files.
namespace MyFilesMove {

// Moves a file, replacing an existing one only when `overwrite`. A failed move never loses either file.
bool move_file(const boost::filesystem::path &from, const boost::filesystem::path &to, bool overwrite, std::string &error);

// The part of move_file() used across volumes: the copy is completed beside `to` before it takes its place, and only
// then is `from` removed.
bool move_by_copy(const boost::filesystem::path &from, const boost::filesystem::path &to, bool overwrite, std::string &error);

} // namespace MyFilesMove
} // namespace Slic3r
