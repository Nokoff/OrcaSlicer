#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Slic3r
{

/**
 * @brief One spool (reel) as reported by a Spoolman server.
 */
struct SpoolmanSpool
{
    int id { 0 };
    std::string vendor;
    std::string filamentName;
    std::string material;
    // Normalized "#RRGGBB" colours; more than one means a multicolour filament.
    std::vector<std::string> colors;
    // True when Spoolman says the colours run along the filament (gradient) rather than side by side.
    bool longitudinal { false };
    double remainingWeight { -1.0 };
    std::string location;
    std::string lotNumber;
    bool archived { false };

    std::string DisplayName() const;
};

/**
 * @brief Minimal read-only client for the Spoolman REST API (https://github.com/Donkie/Spoolman).
 */
class Spoolman
{
public:
    using FetchCallback = std::function<void(std::vector<SpoolmanSpool> spools, std::string error)>;

    // App config key holding the Spoolman base URL, e.g. "http://192.168.1.10:7912".
    static constexpr const char* ConfigKey = "spoolman_url";

    /**
     * @brief Trims whitespace and trailing slashes and adds "http://" when no scheme is given.
     *        Returns an empty string when the value is empty.
     */
    static std::string NormalizeUrl(const std::string& url);

    /**
     * @brief Parses the body of GET /api/v1/spool.
     */
    static bool ParseSpools(const std::string& body, std::vector<SpoolmanSpool>& spools, std::string& error);

    /**
     * @brief Fetches the non-archived spools in a background thread. The callback runs on that thread.
     */
    static void FetchSpools(const std::string& baseUrl, FetchCallback callback);
};

} // namespace Slic3r
