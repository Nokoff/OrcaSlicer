#include "Spoolman.hpp"

#include "Http.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"

#include <algorithm>
#include <utility>

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r
{

namespace
{

constexpr long TimeoutConnect = 5;
constexpr long TimeoutMax = 15;

std::string JsonString(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return {};
    return boost::algorithm::trim_copy(it->get<std::string>());
}

double JsonNumber(const nlohmann::json& object, const char* key, double fallback)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number())
        return fallback;
    return it->get<double>();
}

void AppendColor(std::vector<std::string>& colors, const std::string& value)
{
    const std::string normalized = NormalizeFilamentHexColor(value);
    if (!normalized.empty() && std::find(colors.begin(), colors.end(), normalized) == colors.end())
        colors.emplace_back(normalized);
}

std::vector<std::string> SpoolColors(const nlohmann::json& filament)
{
    std::vector<std::string> colors;

    // Spoolman stores multicolour filaments as a comma separated hex list and leaves color_hex empty.
    std::vector<std::string> parts;
    const std::string multiColors = JsonString(filament, "multi_color_hexes");
    boost::algorithm::split(parts, multiColors, boost::is_any_of(",; "), boost::token_compress_on);
    for (const std::string& part : parts)
        AppendColor(colors, part);

    if (colors.size() < 2)
    {
        colors.clear();
        AppendColor(colors, JsonString(filament, "color_hex"));
    }
    return colors;
}

} // namespace

std::string SpoolmanSpool::DisplayName() const
{
    std::string name = vendor;
    if (!material.empty() && !boost::algorithm::icontains(filamentName, material))
        name += (name.empty() ? "" : " ") + material;
    if (!filamentName.empty())
        name += (name.empty() ? "" : " ") + filamentName;
    if (name.empty())
        name = "Spool #" + std::to_string(id);
    return name;
}

std::string Spoolman::NormalizeUrl(const std::string& url)
{
    std::string value = boost::algorithm::trim_copy(url);
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    if (value.empty())
        return {};
    if (!boost::algorithm::istarts_with(value, "http://") && !boost::algorithm::istarts_with(value, "https://"))
        value = "http://" + value;
    return value;
}

bool Spoolman::ParseSpools(const std::string& body, std::vector<SpoolmanSpool>& spools, std::string& error)
{
    spools.clear();

    nlohmann::json root = nlohmann::json::parse(body, nullptr, false);
    if (root.is_discarded())
    {
        error = "Spoolman returned invalid JSON.";
        return false;
    }

    // Spoolman returns a bare list; tolerate the paged shapes other servers use.
    if (root.is_object())
    {
        if (root.contains("items"))
            root = root["items"];
        else if (root.contains("results"))
            root = root["results"];
    }
    if (!root.is_array())
    {
        error = "Unexpected response from Spoolman: expected a list of spools.";
        return false;
    }

    for (const nlohmann::json& item : root)
    {
        if (!item.is_object())
            continue;

        SpoolmanSpool spool;
        spool.id = static_cast<int>(JsonNumber(item, "id", 0));
        spool.remainingWeight = JsonNumber(item, "remaining_weight", -1.0);
        spool.location = JsonString(item, "location");
        spool.lotNumber = JsonString(item, "lot_nr");
        spool.archived = item.value("archived", false);

        const auto filament = item.find("filament");
        if (filament != item.end() && filament->is_object())
        {
            spool.filamentName = JsonString(*filament, "name");
            spool.material = JsonString(*filament, "material");
            spool.colors = SpoolColors(*filament);
            spool.longitudinal = boost::algorithm::iequals(JsonString(*filament, "multi_color_direction"), "longitudinal");

            const auto vendor = filament->find("vendor");
            if (vendor != filament->end() && vendor->is_object())
                spool.vendor = JsonString(*vendor, "name");
        }

        if (!spool.archived)
            spools.emplace_back(std::move(spool));
    }

    std::sort(spools.begin(), spools.end(), [](const SpoolmanSpool& lhs, const SpoolmanSpool& rhs) {
        const std::string lhsName = boost::algorithm::to_lower_copy(lhs.DisplayName());
        const std::string rhsName = boost::algorithm::to_lower_copy(rhs.DisplayName());
        return lhsName != rhsName ? lhsName < rhsName : lhs.id < rhs.id;
    });
    return true;
}

void Spoolman::FetchSpools(const std::string& baseUrl, FetchCallback callback)
{
    const std::string url = NormalizeUrl(baseUrl);
    if (url.empty())
    {
        callback({}, "No Spoolman server address is set.");
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "Spoolman: fetching spools from " << url;
    Http::get(url + "/api/v1/spool?allow_archived=false")
        .header("Accept", "application/json")
        .timeout_connect(TimeoutConnect)
        .timeout_max(TimeoutMax)
        .on_complete([callback](std::string body, unsigned status) {
            std::vector<SpoolmanSpool> spools;
            std::string error;
            if (!Spoolman::ParseSpools(body, spools, error))
            {
                BOOST_LOG_TRIVIAL(warning) << "Spoolman: " << error << " (HTTP " << status << ")";
                callback({}, error);
                return;
            }
            callback(std::move(spools), {});
        })
        .on_error([callback](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(warning) << "Spoolman: request failed: " << error << " (HTTP " << status << ")";
            if (status != 0)
                error = "HTTP " + std::to_string(status) + ": " + error;
            callback({}, error);
        })
        .perform();
}

} // namespace Slic3r
