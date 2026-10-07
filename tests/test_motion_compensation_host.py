import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import motion_sequence as host


class FakeClock:
    now = 0.0

    def monotonic(self):
        return self.now


class FakeSerial:
    def __init__(self, clock, bad_profile=False, bad_duty=False):
        self.clock = clock
        self.bad_profile = bad_profile
        self.bad_duty = bad_duty
        self.queue = []
        self.commands = []
        self.enabled = False
        self.control = "LOCKED"
        self.end_at = None
        self.closed = False

    def open(self):
        pass

    def close(self):
        self.closed = True

    def flush(self):
        pass

    def status(self):
        duties = [0]*4
        stby = 0
        if self.control != "LOCKED":
            logical = host.MOVES[self.motion][1]
            duties = host.wheel_duties(logical, self.level, self.enabled)
            if self.bad_duty:
                duties[0] = 192
            stby = 1
        return (f"STATUS hw=1 ps=1 mode=41 buttons=0000 control={self.control} "
                f"STBY={stby} duty={','.join(map(str, duties))}")

    def write(self, raw):
        text = raw.decode().strip()
        self.commands.append(text)
        if text == "STOP":
            self.control = "LOCKED"
            self.end_at = None
            self.queue.append("OK STOP LOCKED")
        elif text == "STATUS":
            self.queue.append(self.status())
        elif text == "INFO":
            self.queue.append(host.INFO)
        elif text == "MOTIONINFO":
            self.queue.append(host.MOTIONINFO)
        elif text.startswith("COMP "):
            self.enabled = text == "COMP ON"
            self.queue.append(f"OK COMP {'ON' if self.enabled else 'OFF'}; LOCKED")
        elif text == "COMPINFO":
            row = host.COMPINFO.format(enabled=int(self.enabled))
            self.queue.append(row.replace("start30s-v3", "unknown") if self.bad_profile else row)
        elif text.startswith("MOVEPULSE "):
            _, self.motion, level, duration = text.split()
            self.level = int(level)
            self.control = "MOVEPULSE"
            self.end_at = self.clock.now + int(duration)/1000
            self.queue.append(f"OK MOVEPULSE {self.motion} duty={level} duration={duration}ms fixedDuty=noRamp")
        else:
            raise AssertionError(text)
        return len(raw)

    def readline(self):
        self.clock.now += 0.02
        if self.end_at is not None and self.clock.now >= self.end_at:
            self.end_at = None
            self.control = "LOCKED"
            self.queue.append("JOG DONE/STOP; LOCKED")
        return (self.queue.pop(0)+"\n").encode() if self.queue else b""


class CompensationHostTests(unittest.TestCase):
    def setUp(self):
        repo = Path(__file__).resolve().parents[1]
        self.test_dir = repo / "build"
        assert self.test_dir.resolve().parent == repo
        self.test_dir.mkdir(exist_ok=True)

    def run_host(self, serial, clock, log, compensated=True, moves=("FWD", "BACK")):
        args = ["motion_sequence.py", "--moves", *moves, "--duration", "300",
                "--prepare", "0", "--log", str(log)]
        if compensated:
            args.append("--compensate")
        ports = [SimpleNamespace(device="FAKE", vid=0x1A86, pid=0x7523)]
        with patch.object(sys, "argv", args), patch.object(host.time, "monotonic", clock.monotonic), \
             patch.object(host.serial, "Serial", return_value=serial), \
             patch.object(host.list_ports, "comports", return_value=ports), \
             contextlib.redirect_stdout(io.StringIO()):
            host.main()

    def test_compensation_stays_on_across_moves_and_resets_at_exit(self):
        clock = FakeClock()
        serial = FakeSerial(clock)
        with tempfile.TemporaryDirectory(dir=self.test_dir) as folder:
            log = Path(folder)/"trace.txt"
            self.run_host(serial, clock, log)
            text = log.read_text(encoding="utf-8")
        self.assertIn("duty=123,108,-144,-192", text)
        self.assertIn("duty=-123,-108,144,192", text)
        self.assertEqual(serial.commands.count("COMP ON"), 1)
        self.assertEqual(serial.commands.count("COMP OFF"), 1)
        self.assertFalse(serial.enabled)
        self.assertTrue(serial.closed)
        self.assertEqual(serial.control, "LOCKED")

    def test_profile_mismatch_prevents_any_motion(self):
        clock = FakeClock()
        serial = FakeSerial(clock, bad_profile=True)
        with tempfile.TemporaryDirectory(dir=self.test_dir) as folder:
            with self.assertRaisesRegex(RuntimeError, "profile mismatch"):
                self.run_host(serial, clock, Path(folder)/"trace.txt")
        self.assertFalse(any(c.startswith("MOVEPULSE ") for c in serial.commands))
        self.assertTrue(serial.closed)
        self.assertFalse(serial.enabled)

    def test_wrong_physical_duty_aborts_without_retry_and_stops(self):
        clock = FakeClock()
        serial = FakeSerial(clock, bad_duty=True)
        with tempfile.TemporaryDirectory(dir=self.test_dir) as folder:
            with self.assertRaisesRegex(RuntimeError, "Unexpected motion state"):
                self.run_host(serial, clock, Path(folder)/"trace.txt")
        self.assertEqual(sum(c.startswith("MOVEPULSE ") for c in serial.commands), 1)
        self.assertFalse(serial.enabled)
        self.assertTrue(serial.closed)
        self.assertEqual(serial.control, "LOCKED")

    def test_raw_mode_selects_off(self):
        clock = FakeClock()
        serial = FakeSerial(clock)
        serial.enabled = True
        with tempfile.TemporaryDirectory(dir=self.test_dir) as folder:
            log = Path(folder)/"trace.txt"
            self.run_host(serial, clock, log, compensated=False, moves=("FWD",))
            self.assertIn("duty=192,192,-192,-192", log.read_text(encoding="utf-8"))
        self.assertNotIn("COMP ON", serial.commands)


if __name__ == "__main__":
    unittest.main()
