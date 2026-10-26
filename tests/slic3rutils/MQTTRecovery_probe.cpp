#include "slic3r/Utils/MQTT.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

template<class Predicate> void wait_until(Predicate predicate, std::chrono::seconds timeout, const char* error)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error(error);
        std::this_thread::sleep_for(20ms);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
        return 2;
    try {
        std::atomic<int> responses{0};
        std::atomic<int> failures{0};
        auto             client = std::make_shared<MqttClient>(std::string("tcp://127.0.0.1:") + argv[1], "recovery-test", "", "", true);
        client->SetMessageCallback([&responses](const std::string& topic, const std::string& payload) {
            if (topic == "test/response" && payload == "alive")
                ++responses;
        });
        client->SetConnectionFailureCallback([&failures]() { ++failures; });
        std::string msg;
        if (!client->Connect(msg) || !client->Subscribe("test/response", 1, msg))
            throw std::runtime_error("Initial connection or subscription failed");
        wait_until([&]() { return responses == 1; }, 5s, "Initial response missing");
        for (int expected = 2; expected <= 3; ++expected) {
            if (!client->Publish("test/control", "drop", 1, msg))
                throw std::runtime_error("Drop request failed");
            wait_until([&]() { return responses >= expected; }, 15s, "Response subscription missing after reconnect");
        }
        // Let both old timers expire while connected. Leaked outage counters used
        // to suppress the next failure once these timers had already returned.
        std::this_thread::sleep_for(22s);
        if (failures != 0)
            throw std::runtime_error("Recovered outage timer reported a failure");
        const auto outage_started = std::chrono::steady_clock::now();
        // The third outage must get its own watchdog despite two earlier recoveries.
        if (!client->Publish("test/control", "offline", 1, msg))
            throw std::runtime_error("Offline request failed");
        if (argc == 3 && std::string(argv[2]) == "cancel") {
            wait_until([&]() { return !client->CheckConnected(); }, 5s, "Outage not detected");
            client->Disconnect(msg);
            std::this_thread::sleep_for(22s);
            if (failures != 0)
                throw std::runtime_error("Explicit disconnect did not cancel the watchdog");
            client.reset();
            std::cout << "PASS: explicit disconnect cancels outage watchdog\n";
            return 0;
        }
        wait_until([&]() { return failures > 0; }, 27s, "Repeated outage never reported by watchdog");
        if (std::chrono::steady_clock::now() - outage_started < 19s)
            throw std::runtime_error("An old watchdog interrupted a newer outage");
        if (failures != 1)
            throw std::runtime_error("Stale watchdog reported a duplicate failure");
        client->Disconnect(msg);
        client.reset();
        std::cout << "PASS: two reconnects restore responses; repeated outage reports exactly one failure\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
