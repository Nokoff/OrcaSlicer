#include "MyFilesMove.hpp"

#include <ctime>

#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/system/error_code.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace fs = boost::filesystem;

namespace Slic3r {
namespace MyFilesMove {

namespace {

// A rename that takes effect in one step, and fails across volumes instead of copying over the destination.
void rename_in_one_step(const fs::path &from, const fs::path &to, bool overwrite, boost::system::error_code &ec)
{
#ifdef _WIN32
    // fs::rename() adds MOVEFILE_COPY_ALLOWED, with which a failed copy has already destroyed the destination
    if (::MoveFileExW(from.c_str(), to.c_str(), overwrite ? MOVEFILE_REPLACE_EXISTING : 0))
        ec.clear();
    else
        ec.assign(static_cast<int>(::GetLastError()), boost::system::system_category());
#else
    (void) overwrite; // rename() always replaces, move_file() has already checked the destination
    fs::rename(from, to, ec);
#endif
}

} // namespace

bool move_by_copy(const fs::path &from, const fs::path &to, bool overwrite, std::string &error)
{
    boost::system::error_code ec;
    const fs::path temporary = to.parent_path() / fs::unique_path(".%%%%-%%%%-%%%%.moving", ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    const auto give_up = [&] {
        error = ec.message();
        boost::system::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    };

    fs::copy_file(from, temporary, ec);
    if (ec)
        return give_up();

    // The library sorts by date, so a moved file keeps its own
    boost::system::error_code time_ec;
    const std::time_t modified = fs::last_write_time(from, time_ec);
    if (!time_ec)
        fs::last_write_time(temporary, modified, time_ec);

    rename_in_one_step(temporary, to, overwrite, ec);
    if (ec)
        return give_up();

    fs::remove(from, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

bool move_file(const fs::path &from, const fs::path &to, bool overwrite, std::string &error)
{
    boost::system::error_code ec;
    if (!fs::is_regular_file(from, ec)) {
        error = "The source is not a file";
        return false;
    }

    boost::system::error_code status_ec;
    const fs::file_status     target = fs::symlink_status(to, status_ec);
    if (fs::exists(target)) {
        if (fs::is_directory(target) || !overwrite) {
            error = "The destination already exists";
            return false;
        }
        if (fs::equivalent(from, to, ec) && !ec) {
            error = "The source and the destination are the same file";
            return false;
        }
    }

    rename_in_one_step(from, to, overwrite, ec);
    if (!ec)
        return true;

#ifdef _WIN32
    const bool other_volume = ec.value() == ERROR_NOT_SAME_DEVICE;
#else
    const bool other_volume = ec == boost::system::errc::cross_device_link;
#endif
    if (!other_volume) {
        error = ec.message();
        return false;
    }
    return move_by_copy(from, to, overwrite, error);
}

} // namespace MyFilesMove
} // namespace Slic3r
