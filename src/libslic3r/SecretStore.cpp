#include "SecretStore.hpp"

#include <array>
#include <cstdint>
#include <vector>

#include <boost/log/trivial.hpp>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace Slic3r {

namespace {

#ifdef _WIN32

// Marks a value as sealed so plain values written by older builds stay readable.
const char SEALED_PREFIX[] = "dpapi:v1:";

const char BASE64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char *data, size_t size)
{
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const size_t   remaining = size - i;
        const uint32_t triple    = (uint32_t(data[i]) << 16) | (remaining > 1 ? uint32_t(data[i + 1]) << 8 : 0) |
                                (remaining > 2 ? uint32_t(data[i + 2]) : 0);
        out += BASE64_ALPHABET[(triple >> 18) & 0x3F];
        out += BASE64_ALPHABET[(triple >> 12) & 0x3F];
        out += remaining > 1 ? BASE64_ALPHABET[(triple >> 6) & 0x3F] : '=';
        out += remaining > 2 ? BASE64_ALPHABET[triple & 0x3F] : '=';
    }
    return out;
}

bool base64_decode(const std::string &in, std::vector<unsigned char> &out)
{
    if (in.empty() || in.size() % 4 != 0)
        return false;

    std::array<int, 256> lookup;
    lookup.fill(-1);
    for (int i = 0; i < 64; ++i)
        lookup[static_cast<unsigned char>(BASE64_ALPHABET[i])] = i;

    out.clear();
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int    values[4] = {0, 0, 0, 0};
        size_t padding   = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = in[i + j];
            if (c == '=') {
                ++padding;
                continue;
            }
            if (padding != 0)
                return false;
            values[j] = lookup[static_cast<unsigned char>(c)];
            if (values[j] < 0)
                return false;
        }
        if (padding > 2)
            return false;

        const uint32_t triple = (uint32_t(values[0]) << 18) | (uint32_t(values[1]) << 12) | (uint32_t(values[2]) << 6) |
                                uint32_t(values[3]);
        out.push_back(static_cast<unsigned char>((triple >> 16) & 0xFF));
        if (padding < 2)
            out.push_back(static_cast<unsigned char>((triple >> 8) & 0xFF));
        if (padding < 1)
            out.push_back(static_cast<unsigned char>(triple & 0xFF));
    }
    return true;
}

#endif // _WIN32

} // namespace

std::string encrypt_secret(const std::string &plain)
{
    if (plain.empty())
        return plain;

#ifdef _WIN32
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());

    DATA_BLOB sealed{};
    if (!CryptProtectData(&in, L"Snapmaker Orca device credential", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &sealed)) {
        BOOST_LOG_TRIVIAL(warning) << "SecretStore: unable to seal credential, dropping it instead of storing it in the clear";
        return {};
    }

    std::string out = SEALED_PREFIX + base64_encode(sealed.pbData, sealed.cbData);
    SecureZeroMemory(sealed.pbData, sealed.cbData);
    LocalFree(sealed.pbData);
    return out;
#else
    BOOST_LOG_TRIVIAL(warning) << "SecretStore: no keystore on this platform, credential not stored";
    return {};
#endif
}

std::string decrypt_secret(const std::string &stored)
{
    if (stored.empty())
        return stored;

#ifdef _WIN32
    const size_t prefix_len = sizeof(SEALED_PREFIX) - 1;
    if (stored.compare(0, prefix_len, SEALED_PREFIX) != 0)
        return stored;

    std::vector<unsigned char> raw;
    if (!base64_decode(stored.substr(prefix_len), raw)) {
        BOOST_LOG_TRIVIAL(warning) << "SecretStore: stored credential is malformed";
        return {};
    }

    DATA_BLOB in;
    in.pbData = raw.data();
    in.cbData = static_cast<DWORD>(raw.size());

    DATA_BLOB opened{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &opened)) {
        BOOST_LOG_TRIVIAL(warning) << "SecretStore: stored credential cannot be read by this user account";
        return {};
    }

    std::string plain(reinterpret_cast<char *>(opened.pbData), opened.cbData);
    SecureZeroMemory(opened.pbData, opened.cbData);
    LocalFree(opened.pbData);
    return plain;
#else
    return stored;
#endif
}

} // namespace Slic3r
