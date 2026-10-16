import unittest
import queue
import time
from unittest.mock import patch
from protocol import command, parse
from linux_protocol import encode, crc16, Decoder, describe
from channel_panel import pwm
from linux_panel import RawSerial


class MultiTests(unittest.TestCase):
    def test_batch_and_boundaries(self):
        values = (500, -600, 700, -800, 900, -1000, 800, -800)
        line = command("MSET", 9, 255, values, (500, 600, 700, 800, 900, 1000, 0, 0))
        self.assertEqual(len(line.split()), 19)
        self.assertLess(len(line), 192)
        self.assertEqual(command("MOFF", 1), "MOFF 1\n")
        for index, value in ((0, 499), (6, 801), (7, -1000)):
            bad = list(values); bad[index] = value
            with self.assertRaises(ValueError): command("MSET", 0, 255, bad, (0,)*8)
        with self.assertRaises(ValueError): command("MSET", 0, 0, values, (0,)*8)
        with self.assertRaises(ValueError): command("MSET", 0, 255, values, (60001,)*8)
        self.assertEqual(command("LSET", 4, duty=(0, 1, 250, 500, 750, 999, 1000)), "LSET 4 0 1 250 500 750 999 1000\n")
        with self.assertRaises(ValueError): command("LSET", 0, duty=(1001,)*7)
        self.assertEqual(pwm("50.1"), 501)
        for text in ("nan", "inf", "-1", "100.1", "50.01"):
            with self.assertRaises(ValueError): pwm(text)

    def test_states_and_bridge(self):
        line = "MSTATE 100 500 -600 0 0 0 0 800 -800 100 200 0 0 0 0 0 0 4 0"
        state = parse(line)
        self.assertEqual(state[0], "MSTATE")
        self.assertEqual(state[2], (500, -600, 0, 0, 0, 0, 800, -800))
        self.assertEqual(state[4], 4)
        self.assertEqual(parse("LSTATE 100 0 10 20 30 40 50 1000 1")[3], True)
        self.assertEqual(command("LTX", 3, duty=b"\x00\xff\n"), "LTX 3 00FF0A\n")
        self.assertEqual(parse("LRX 00FF0A"), ("LRX", b"\x00\xff\n"))
        for bad in ("LRX GG", "LSTATE 1 0 0 0 0 0 0 1001 0", line.replace(" 4 0", " 256 0")):
            self.assertEqual(parse(bad)[0], "BAD")


class LinuxTests(unittest.TestCase):
    def test_independent_raw_serial(self):
        class FakeRaw:
            def __init__(self): self.writes = []; self.closed = False
            def __enter__(self): return self
            def __exit__(self, *_): self.closed = True
            def write(self, data): self.writes.append(data); return len(data)
            def read(self, _): time.sleep(.005); return b"\xe4\xb8\xad\xff\x00"
        fake, events = FakeRaw(), queue.Queue(maxsize=100)
        settings = dict(baudrate=57600, bytesize=8, parity="N", stopbits=1)
        with patch("serial.Serial", return_value=fake) as factory:
            worker = RawSerial("TEST_COM", settings, events)
            worker.commands.put(b"\x00\xff\n")
            worker.start()
            deadline = time.monotonic()+1
            received = []
            while time.monotonic() < deadline:
                kind, data = events.get(timeout=1)
                received.append(kind)
                if kind == "rx": break
            worker.quit.set(); worker.join(1)
            self.assertFalse(worker.is_alive())
            self.assertTrue(fake.closed)
            self.assertEqual(fake.writes, [b"\x00\xff\n"])
            self.assertIn("tx", received)
            factory.assert_called_once_with("TEST_COM", timeout=.02, write_timeout=.2, **settings)

    def test_reference_vectors(self):
        self.assertEqual(crc16(b"123456789"), 0x4b37)
        raw = encode(4, 0x20, flags=0)
        self.assertEqual(raw.hex(), "a55a0200042000000103")
        decoder = Decoder()
        self.assertEqual(decoder.feed(raw[:6]), [])
        self.assertEqual(decoder.feed(raw[6:]), [(4, 0x20, 0, b"")])

    def test_noise_bad_crc_and_multiple_frames(self):
        raw = encode(17, 0x7f, bytes((0x10, 0, 0, 0)), 1)
        decoder = Decoder()
        bad = raw[:-1]+bytes((raw[-1]^1,))
        frames = decoder.feed(b"junk"+bad+raw+raw)
        self.assertEqual(len(frames), 2)
        self.assertGreater(decoder.errors, 0)
        self.assertIn("ACK", describe(frames[0]))
        self.assertIn("接受成功", describe(frames[0]))
        status = (18, 0x21, 1, bytes((1, 80, 0, 0, 0, 0, 0, 0))+bytes(8))
        self.assertIn("亮度=80%", describe(status))


if __name__ == "__main__": unittest.main()
