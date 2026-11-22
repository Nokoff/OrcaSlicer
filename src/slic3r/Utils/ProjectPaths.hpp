#pragma once

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>

namespace Slic3r {
namespace ProjectPaths {

// The folder Save writes to without asking, for a project named after `file`. Only a 3MF opened with its settings has
// one: part.stl would be saved over part.3mf, and a 3MF imported as geometry over itself, losing its settings. A G-code
// preview, which cannot be saved, keeps its folder.
inline boost::filesystem::path project_folder(const boost::filesystem::path& file, bool gcode_preview, bool geometry_only = false)
{
    if (gcode_preview || (!geometry_only && boost::algorithm::iends_with(file.extension().string(), ".3mf")))
        return file.parent_path();
    return {};
}

// Whether importing a model may name the project after it. A saved project keeps its name, as that is also the
// file Save writes to.
inline bool import_may_rename(const boost::filesystem::path& folder, bool untitled)
{
    return untitled && folder.empty();
}

// Stops crash recovery from making the file that was opened the one Save writes to.
inline void forget_origin(const boost::filesystem::path& backup)
{
    boost::system::error_code ec;
    boost::filesystem::remove(backup / "origin.txt", ec);
}

} // namespace ProjectPaths
} // namespace Slic3r
