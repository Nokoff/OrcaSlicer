# Network instability investigation

Investigated 25 October 2026. Application log timestamps below use Africa/Johannesburg.

## Evidence

- Source label: build-profile logs 2026-10-25. The 09:52 and 12:55 application sessions overlapped and recorded 2561 and 1045 MQTT connection losses respectively. Near the end they repeatedly lost connection on alternating roughly 6.5-second cycles. Their logs contain shared MQTT client-ID field values. Competing client identities are a likely contributor; broker-side confirmation is still needed. The subsequent 17:16 session had no native MQTT losses in the inspected log.
- Source label: Snapmaker log 2026-10-23. Heartbeat replies initially completed in tens of milliseconds. At 14:28:24 a heartbeat exhausted both three-second attempts. At 14:28:43 the native MQTT transport reported a lost connection. Later heartbeat requests continued to time out.
- Source label: Snapmaker log 2026-10-21. The heartbeat exception counter reached 1089. Some later exceptions were skipped background requests, so that counter is not a measure of physical packet loss.
- Logs were read from the user application log directory and build/src/Release/data_dir/log. No raw log payloads, printer identifiers, addresses, or credentials are copied here.

## Confirmed implementation defects

1. `MqttClient` passed reconnect intervals of zero to Paho. Source label: Paho connect_options.cpp, set_automatic_reconnect(int, int). That overload enables reconnect rather than disabling it. Changing the options object in connected() does not update the options already copied into the C transport. Configure the intended 2–30 second backoff before connecting.
2. The existing resubscription function was never called after reconnect. A restarted broker can discard subscriptions while the application reports a connected socket. Restore remembered response/status subscriptions after reconnect. Queue subscriptions asynchronously because the connection callback runs on the thread that processes SUBACKs. Protect the subscription map with a mutex.
3. The outage counter could remain incremented after successful recovery, suppressing the failure callback during a later outage. Replace it with an atomic generation. A timer can only claim its own outage; reconnect and explicit disconnect invalidate old timers. This also removes unnecessary destructor waits for sleeping weak-pointer timers.
4. The clean_session constructor argument was ignored. Pass it through to Paho. The default remains false.

## Validation

- Built mqtt_recovery_probe in Release, compiling the production MQTT.cpp against the bundled Paho libraries.
- CTest mqtt_recovery passed: two forced drops restore response delivery with broker session state discarded; after the old timers expire, a third sustained outage reports one failure at its own deadline.
- CTest mqtt_recovery_cancel passed: explicit disconnect during an outage cancels the timer.
- Both fixtures also verify clean-session flags and nonzero reconnect backoff. Tests use disposable loopback brokers and require no printer or credentials.
- Scoped git diff --check passed.
- Full application sources compiled in Release. The running slicer locked the default output DLL, so the application was successfully linked into build/network_recovery_release instead. The packaged launcher passed a hidden --help smoke check with exit code 0.

Runnable build: [snapmaker-orca.exe](../build/network_recovery_release/snapmaker-orca.exe). Close other printer-connected slicer instances before validating recovery. The build uses the existing working tree, including pre-existing edits.

## Files changed

- src/slic3r/Utils/MQTT.cpp
- src/slic3r/Utils/MQTT.hpp
- tests/slic3rutils/CMakeLists.txt
- tests/slic3rutils/MQTTRecovery_probe.cpp
- tests/slic3rutils/mqtt_recovery_broker.py
- doc/network-instability.md

Build: `cmake --build build --config Release --target mqtt_recovery_probe`

Test: `ctest --test-dir build -C Release -R ^mqtt_recovery --output-on-failure --parallel 2`

## Limits and follow-up

These changes improve recovery after a transport interruption. The concurrent-session evidence suggests client-ID contention, but the logs alone cannot establish the cause of every TCP drop. Physical printer/Wi-Fi validation has not been performed. The existing 20-second outage deadline remains in place.

Manual verification: keep one slicer instance connected to the printer, run the rebuilt application, connect to a printer, interrupt and restore the network twice, and confirm live status and commands recover. Deliberately disconnect during an interruption and confirm no later failure notification appears.
