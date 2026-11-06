#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string read_flutter_bundle()
{
    // Installer staging can replace the development assets with a pinned,
    // content-hashed bundle. Validate that exact shipping entrypoint when set.
    const char* staged_bundle = std::getenv("ORCA_TEST_FLUTTER_BUNDLE");
    const char* bundle_path = staged_bundle && *staged_bundle ? staged_bundle : GATEWAY_FLUTTER_BUNDLE;
    INFO("Flutter bundle: " << bundle_path);
    std::ifstream bundle(bundle_path, std::ios::binary);
    REQUIRE(bundle.good());
    return std::string{std::istreambuf_iterator<char>{bundle}, {}};
}

} // namespace

TEST_CASE("shipped Flutter bundle does not use removed device bridge commands", "[gateway][flutter]")
{
    const std::string bundle = read_flutter_bundle();
    for (const char* command : {
             "sw_SystemGetDeviceInfo",
             "sw_GetMachineState",
             "sw_SubscribeMachineState",
             "sw_GetMachineObjects",
             "sw_SetSubscribeFilter",
             "sw_StopMachineStateSubscription",
             "sw_GetPrinterInfo",
             "sw_GetMachineSystemInfo",
         })
        CHECK(bundle.find(command) == std::string::npos);
}
