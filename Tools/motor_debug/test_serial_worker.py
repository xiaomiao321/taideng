import queue
import threading
import time
import unittest
from unittest.mock import patch
from app import SerialWorker


class FakeSerial:
    def __init__(self, omit=None):
        self.omit = omit
        self.writes = []
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.closed = False

    def reset_input_buffer(self):
        pass

    def write(self, data):
        parts = data.decode().split()
        with self.lock:
            self.writes.append(parts)
            if parts[0] != self.omit:
                result = "QUEUED" if parts[0] == "RUN" else parts[0]
                self.buf.extend(f"ACK {parts[1]} {result}\r\n".encode())
                if parts[0] == "STOP":
                    self.buf.extend(b"STATE 123 0 0 0 0\r\nPOT 123 1 2 3 4 5 6\r\n")
        return len(data)

    def read(self, _):
        time.sleep(0.005)
        with self.lock:
            result = bytes(self.buf[:7])  # deliberately split lines across reads
            del self.buf[:7]
        return result

    def close(self):
        self.closed = True


def wait_event(events, kind, timeout=2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            event = events.get(timeout=0.05)
        except queue.Empty:
            continue
        if event[0] == kind:
            return event
    raise AssertionError(f"No {kind} event")


class SerialTests(unittest.TestCase):
    def test_handshake_fragmented_lines_and_disconnect(self):
        fake, events = FakeSerial(), queue.Queue()
        with patch("serial.Serial", return_value=fake):
            worker = SerialWorker("FAKE", events)
            worker.start()
            try:
                wait_event(events, "connected")
                worker.submit("RUN", 1, -800, 500)
                end = time.monotonic() + 1
                while not any(w[0] == "RUN" for w in fake.writes) and time.monotonic() < end:
                    time.sleep(.01)
                self.assertEqual(sum(w[0] == "RUN" for w in fake.writes), 1)
            finally:
                worker.quit.set()
                worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertTrue(fake.closed)
            self.assertEqual(fake.writes[:2], [["PING", "1"], ["STOP", "2", "ALL"]])
            self.assertEqual(fake.writes[-1][0], "STOP")

    def test_no_ack_fails_closed(self):
        fake, events = FakeSerial(omit="PING"), queue.Queue()
        with patch("serial.Serial", return_value=fake):
            worker = SerialWorker("FAKE", events)
            worker.start()
            wait_event(events, "error")
            worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertTrue(fake.closed)
            self.assertEqual(fake.writes[-1][0], "STOP")
            self.assertFalse(any(w[0] == "RUN" for w in fake.writes))

    def test_run_not_retried_on_lost_ack(self):
        fake, events = FakeSerial(omit="RUN"), queue.Queue()
        with patch("serial.Serial", return_value=fake):
            worker = SerialWorker("FAKE", events)
            worker.start()
            wait_event(events, "connected")
            worker.submit("RUN", 8, 800, 0)
            wait_event(events, "error")
            worker.join(2)
            self.assertEqual(sum(w[0] == "RUN" for w in fake.writes), 1)
            self.assertEqual(fake.writes[-1][0], "STOP")


if __name__ == "__main__":
    unittest.main()
