#ifndef slic3r_SecretStore_hpp_
#define slic3r_SecretStore_hpp_

#include <string>

namespace Slic3r {

// Protects credentials that have to survive in the application config file.
//
// On Windows the value is sealed with DPAPI, so it can only be read back by the
// same Windows user account on the same machine. No equivalent keystore is
// wired up on the other platforms yet, and rather than writing a private key
// out in the clear the value is dropped there; callers must cope with getting
// an empty string back and fall back to asking the user to connect manually.
std::string encrypt_secret(const std::string &plain);

// Reverses encrypt_secret(). Values written before sealing existed are returned
// unchanged, so older config files keep working.
std::string decrypt_secret(const std::string &stored);

} // namespace Slic3r

#endif // slic3r_SecretStore_hpp_
