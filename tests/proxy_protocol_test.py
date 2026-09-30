"""Loopback regression tests for the proxy wire protocol and SDK lease renewal."""

import hashlib
import hmac
from pathlib import Path
import socket
import struct
import subprocess
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROXY = ROOT / "server/proxyserver/bin/p2p_proxy"
PEER = ROOT / "client/bin/peer"
HEADER = struct.Struct(">HBBI")
# Existing public demo protocol key, not a production credential.
DEMO_KEY = b"p2p-proxy-auth-2024"


def packet(kind, payload=b""):
    return HEADER.pack(0x584E, 2, kind, len(payload)) + payload


def uuid_field(uuid):
    return uuid.encode().ljust(33, b"\0")


def registration(uuid):
    return uuid_field(uuid) + hmac.digest(DEMO_KEY, uuid.encode(), hashlib.sha256)


def relay(source, target, payload=b"test"):
    return packet(0x23, uuid_field(source) + uuid_field(target) + payload)


class SocketTest(unittest.TestCase):
    def udp(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(sock.close)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(1)
        return sock

    def spawn(self, *args):
        process = subprocess.Popen(
            [str(arg) for arg in args],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )

        def stop():
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)

        self.addCleanup(stop)
        return process

    def receive(self, sock, kind):
        data, address = sock.recvfrom(65535)
        self.assertGreaterEqual(len(data), HEADER.size)
        magic, version, actual_kind, length = HEADER.unpack_from(data)
        self.assertEqual((magic, version, actual_kind), (0x584E, 2, kind))
        self.assertEqual(length, len(data) - HEADER.size)
        return data[HEADER.size:], address

    def silent(self, sock):
        old_timeout = sock.gettimeout()
        sock.settimeout(0.15)
        try:
            with self.assertRaises(socket.timeout):
                sock.recvfrom(65535)
        finally:
            sock.settimeout(old_timeout)


class ProxyProtocolTest(SocketTest):
    def setUp(self):
        reservation = self.udp()
        self.proxy_address = reservation.getsockname()
        reservation.close()
        self.process = self.spawn(PROXY, self.proxy_address[1], 2, 4)
        self.a, self.b, self.other = self.udp(), self.udp(), self.udp()
        deadline = time.monotonic() + 5
        self.a.settimeout(0.1)
        while True:
            self.assertIsNone(self.process.poll(), "proxy exited during startup")
            try:
                self.status(self.a)
                break
            except socket.timeout:
                if time.monotonic() >= deadline:
                    self.fail("proxy did not become ready")
        self.a.settimeout(1)

    def status(self, sock):
        sock.sendto(packet(0x10, b"\0" * 18), self.proxy_address)
        payload, _ = self.receive(sock, 0x11)
        return struct.unpack(">BHH", payload)

    def register(self, sock, uuid, expected=0):
        sock.sendto(packet(0x20, registration(uuid)), self.proxy_address)
        payload, _ = self.receive(sock, 0x21)
        self.assertEqual(len(payload), 19)
        self.assertEqual(payload[0], expected)

    def test_source_binding_and_helper(self):
        self.register(self.a, "A")
        self.register(self.b, "B")
        self.assertEqual(self.status(self.a), (0, 2, 2))
        for source in ("A", "B", "UNKNOWN"):
            self.other.sendto(relay(source, "B"), self.proxy_address)
            self.silent(self.b)
        self.a.sendto(relay("B", "B"), self.proxy_address)
        self.silent(self.b)
        valid = relay("A", "B")
        self.a.sendto(valid, self.proxy_address)
        data, _ = self.b.recvfrom(4096)
        self.assertEqual(data, valid)
        helper = packet(0x24, uuid_field("A") + uuid_field("B"))
        self.other.sendto(helper, self.proxy_address)
        self.silent(self.other)
        self.a.sendto(helper, self.proxy_address)
        payload, _ = self.receive(self.a, 0x21)
        self.assertEqual(payload[0], 0)
        self.assertEqual(struct.unpack(">H", payload[-2:])[0], self.b.getsockname()[1])
        self.other.sendto(packet(0x22, registration("A")), self.proxy_address)
        self.assertEqual(self.status(self.other)[1], 2)
        self.a.sendto(packet(0x22, registration("A")), self.proxy_address)
        self.assertEqual(self.status(self.a)[1], 1)
        self.a.sendto(valid, self.proxy_address)
        self.silent(self.b)

    def test_migration_and_capacity(self):
        self.register(self.a, "A")
        self.register(self.b, "B")
        self.register(self.other, "C", expected=1)
        self.register(self.other, "A")
        self.register(self.a, "C", expected=1)
        self.a.sendto(relay("A", "B"), self.proxy_address)
        self.silent(self.b)
        self.other.sendto(relay("A", "B"), self.proxy_address)
        self.receive(self.b, 0x23)
        self.register(self.other, "C")
        self.other.sendto(relay("C", "B"), self.proxy_address)
        self.receive(self.b, 0x23)
        self.register(self.b, "C")
        self.assertEqual(self.status(self.b)[1], 1)
        self.register(self.other, "D")
        self.b.sendto(relay("C", "D"), self.proxy_address)
        self.receive(self.other, 0x23)

    def test_malformed_and_oversized_datagrams(self):
        self.register(self.a, "A")
        self.register(self.b, "B")
        valid_registration = registration("A")
        invalid_registration = valid_registration[:-1] + bytes([valid_registration[-1] ^ 1])
        self.other.sendto(packet(0x20, invalid_registration), self.proxy_address)
        payload, _ = self.receive(self.other, 0x21)
        self.assertEqual(payload[0], 3)
        invalid = [
            packet(0x20, b"x" * 33 + b"\0" * 32),
            packet(0x20, registration("C")[:-1]),
            packet(0x20, registration("C") + b"x"),
        ]
        for data in invalid:
            self.other.sendto(data, self.proxy_address)
            self.silent(self.other)
        maximum = relay("A", "B", b"x" * (2048 - 8 - 66))
        invalid = [
            relay("A", "B") + b"trailing",
            relay("A", "B")[:-1],
            maximum + b"x" * 2048,
            packet(0x23, b"A" * 33 + uuid_field("B") + b"x"),
            packet(0x23, uuid_field("A") + b"B" * 33 + b"x"),
            packet(0x23, b"short"),
        ]
        for data in invalid:
            self.a.sendto(data, self.proxy_address)
            self.silent(self.b)
        self.a.sendto(maximum, self.proxy_address)
        data, _ = self.b.recvfrom(4096)
        self.assertEqual(data, maximum)
        self.assertEqual(self.status(self.other)[1], 2)


class ClientLeaseTest(SocketTest):
    def test_ack_validation_and_idle_renewal(self):
        nat, proxy, unrelated = self.udp(), self.udp(), self.udp()
        client = self.spawn(
            PEER, "127.0.0.1", nat.getsockname()[1], "LEASE",
            "127.0.0.1", proxy.getsockname()[1],
        )
        proxy.settimeout(5)
        body, address = self.receive(proxy, 0x20)
        self.assertEqual(body, registration("LEASE"))
        success = packet(0x21, b"\0" * 19)
        unrelated.sendto(success, address)
        self.receive(proxy, 0x20)  # An unrelated endpoint cannot stop retries.
        proxy.sendto(packet(0x21, b"\0"), address)
        self.receive(proxy, 0x20)  # A truncated reply cannot stop retries.
        proxy.sendto(success, address)
        start = time.monotonic()
        proxy.settimeout(35)
        body, refreshed_address = self.receive(proxy, 0x20)
        self.assertGreaterEqual(time.monotonic() - start, 25)
        self.assertEqual(body, registration("LEASE"))
        self.assertEqual(refreshed_address, address)
        self.assertIsNone(client.poll())
        proxy.sendto(packet(0x21, b"\1" + b"\0" * 18), address)
        proxy.settimeout(3)
        self.receive(proxy, 0x20)  # Renewal rejection resumes registration retries.


if __name__ == "__main__":
    unittest.main()
