"""Exercise the real MQTT transport against a disposable loopback MQTT 3.1.1 broker."""
import socket
import struct
import subprocess
import sys
import threading
import time


def read_exact(conn, count):
    data = bytearray()
    while len(data) < count:
        part = conn.recv(count - len(data))
        if not part:
            raise EOFError
        data.extend(part)
    return bytes(data)


def read_packet(conn):
    header = read_exact(conn, 1)[0]
    size = 0
    multiplier = 1
    for _ in range(4):
        value = read_exact(conn, 1)[0]
        size += (value & 127) * multiplier
        if not value & 128:
            return header, read_exact(conn, size)
        multiplier *= 128
    raise ValueError("Invalid MQTT remaining length")


def main():
    stop = threading.Event()
    errors = []
    reconnect_delays = []
    clean_sessions = []
    subscriptions = []
    offline = False
    dropped_at = None
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    listener.settimeout(0.2)

    def serve():
        nonlocal offline, dropped_at
        while not stop.is_set():
            try:
                conn, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with conn:
                conn.settimeout(0.2)
                if offline:
                    continue
                try:
                    while not stop.is_set():
                        try:
                            header, body = read_packet(conn)
                        except socket.timeout:
                            continue
                        kind = header >> 4
                        if kind == 1:  # CONNECT; deliberately never resume a session.
                            clean_sessions.append(bool(body[7] & 2))
                            if dropped_at is not None:
                                reconnect_delays.append(time.monotonic() - dropped_at)
                                dropped_at = None
                            conn.sendall(b"\x20\x02\x00\x00")
                        elif kind == 8:  # SUBSCRIBE and a response proving delivery works.
                            length = struct.unpack("!H", body[2:4])[0]
                            topic = body[4:4 + length]
                            subscriptions.append(topic)
                            conn.sendall(b"\x90\x03" + body[:2] + b"\x01")
                            payload = struct.pack("!H", len(topic)) + topic + b"alive"
                            conn.sendall(b"\x30" + bytes([len(payload)]) + payload)
                        elif kind == 3:  # PUBLISH control request.
                            length = struct.unpack("!H", body[:2])[0]
                            offset = 2 + length
                            if (header >> 1) & 3:
                                conn.sendall(b"\x40\x02" + body[offset:offset + 2])
                                offset += 2
                            offline = body[offset:] == b"offline"
                            dropped_at = time.monotonic()
                            break
                        elif kind == 12:
                            conn.sendall(b"\xd0\x00")
                        elif kind == 14:
                            break
                        else:
                            raise ValueError("Unexpected MQTT packet type: " + str(kind))
                except (EOFError, ConnectionError, OSError):
                    pass
                except Exception as exc:
                    errors.append(str(exc))

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        result = subprocess.run([sys.argv[1], str(listener.getsockname()[1]), *sys.argv[2:]], capture_output=True, text=True, timeout=65)
    finally:
        stop.set()
        listener.close()
        worker.join(timeout=2)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-4000:])
    if errors:
        raise AssertionError(errors)
    if result.returncode:
        return result.returncode
    assert len(subscriptions) == 3, subscriptions
    assert all(clean_sessions), "clean_session argument was ignored"
    assert len(reconnect_delays) == 2, reconnect_delays
    assert all(delay >= 1 for delay in reconnect_delays), reconnect_delays
    print("PASS: clean sessions honored; reconnect backoff observed; subscriptions restored without a saved broker session")
    return 0


if __name__ == "__main__":
    sys.exit(main())
