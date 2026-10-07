import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import pulse_once as host
from test_motion_compensation_host import FakeClock, FakeSerial


class StartupSerial(FakeSerial):
    def __init__(self, clock, bad_profile=False, missing_start=False, missing_hold=False, all_wheels=False, bad_trim_profile=False):
        super().__init__(clock)
        self.bad_profile = bad_profile
        self.missing_start = missing_start
        self.missing_hold = missing_hold
        self.start_at = 0.0
        self.last_report = 0.0
        self.all_wheels = all_wheels
        self.bad_trim_profile = bad_trim_profile

    def status(self):
        value = 0
        if self.control in ("STARTPULSE", "STARTMOVE"):
            elapsed = self.clock.now - self.start_at
            value = -255 if elapsed < .3 and not self.missing_start else -192
            if elapsed >= .3 and self.missing_hold:
                value = -191
        duties = [-value,-value,value,value] if self.all_wheels else [0,0,0,value]
        if self.all_wheels and self.control == "STARTMOVE" and elapsed >= .3 and self.enabled:
            duties = host.wheel_duties((1,)*4,192,True)
            if self.missing_hold:
                duties[0] += 1
        return (f"STATUS hw=1 ps=1 mode=41 buttons=0000 control={self.control} "
                f"STBY={int(value != 0)} duty={','.join(map(str,duties))}")

    def write(self, raw):
        text = raw.decode().strip()
        if text == "COMPINFO" and self.bad_trim_profile:
            self.commands.append(text)
            self.queue.append(host.COMPINFO.format(enabled=int(self.enabled)).replace("start30s-v3","unknown"))
        elif text in ("STARTPULSEINFO", "STARTMOVEINFO"):
            self.commands.append(text)
            revision = "unknown" if self.bad_profile else ("kick300-fwd-v1" if self.all_wheels else "kick300-v1")
            limits = "maxDuty=192 minMs=500 maxMs=30000" if self.all_wheels else "maxDuty=255 minMs=500 maxMs=2000"
            self.queue.append(f"{text} revision={revision} startDuty=255 startMs=300 {limits}")
        elif text.startswith(("STARTPULSE ","STARTMOVE ")):
            self.commands.append(text)
            self.control = "STARTMOVE" if self.all_wheels else "STARTPULSE"
            self.start_at = self.clock.now
            self.last_report = self.clock.now
            self.end_at = self.clock.now + 2.0
            name = "FWD" if self.all_wheels else "RR"
            self.queue.append(f"OK {self.control} {name} duty=192 duration=2000ms startDuty=255 startMs=300")
        else:
            return super().write(raw)
        return len(raw)

    def readline(self):
        self.clock.now += .02
        if self.end_at is not None:
            if self.clock.now >= self.end_at:
                self.end_at = None
                self.control = "LOCKED"
                self.queue.append("JOG DONE/STOP; LOCKED")
            elif self.clock.now - self.last_report >= .5:
                self.last_report = self.clock.now
                self.queue.append(self.status())
        return (self.queue.pop(0) + "\n").encode() if self.queue else b""


class StartupHostTests(unittest.TestCase):
    def run_host(self, serial, clock, log, compensated=False):
        args = ["pulse_once.py", "--wheel", "ALL" if serial.all_wheels else "RR", "--duty", "192", "--duration", "2000",
                "--startup", "--prepare", "0", "--log", str(log)]
        if compensated:
            args.append("--compensate")
        ports = [SimpleNamespace(device="FAKE", vid=0x1A86, pid=0x7523)]
        with patch.object(sys, "argv", args), patch.object(host.time, "monotonic", clock.monotonic), \
             patch.object(host.serial, "Serial", return_value=serial), \
             patch.object(host.list_ports, "comports", return_value=ports), \
             contextlib.redirect_stdout(io.StringIO()):
            host.main()

    def test_accepts_both_stages_and_stops(self):
        clock = FakeClock()
        serial = StartupSerial(clock)
        folder = Path(__file__).resolve().parents[1] / "build"
        folder.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=folder) as tmp:
            log = Path(tmp) / "trace.txt"
            self.run_host(serial, clock, log)
            text = log.read_text(encoding="utf-8")
            self.assertIn("duty=0,0,0,-255", text)
            self.assertIn("duty=0,0,0,-192", text)
        self.assertEqual(serial.commands.count("STARTPULSE RR 192 2000"), 1)
        self.assertTrue(serial.closed)
        self.assertEqual(serial.control, "LOCKED")

    def test_unknown_capability_prevents_motion(self):
        clock = FakeClock()
        serial = StartupSerial(clock, bad_profile=True)
        folder = Path(__file__).resolve().parents[1] / "build"
        folder.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=folder) as tmp:
            with self.assertRaisesRegex(RuntimeError, "capability"):
                self.run_host(serial, clock, Path(tmp) / "trace.txt")
        self.assertFalse(any(c.startswith("STARTPULSE ") for c in serial.commands))
        self.assertTrue(serial.closed)

    def test_four_wheel_startup_forces_raw_hold_and_checks_stages(self):
        clock = FakeClock()
        serial = StartupSerial(clock, all_wheels=True)
        serial.enabled = True
        folder = Path(__file__).resolve().parents[1] / "build"
        folder.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=folder) as tmp:
            log = Path(tmp) / "trace.txt"
            self.run_host(serial, clock, log)
            text = log.read_text(encoding="utf-8")
            self.assertIn("duty=255,255,-255,-255", text)
            self.assertIn("duty=192,192,-192,-192", text)
        self.assertEqual(serial.commands.count("STARTMOVE FWD 192 2000"), 1)
        self.assertFalse(serial.enabled)
        self.assertTrue(serial.closed)
        self.assertEqual(serial.control,"LOCKED")

    def test_missing_either_stage_aborts_without_retry(self):
        for missing_start in (True, False):
            clock = FakeClock()
            serial = StartupSerial(clock, missing_start=missing_start, missing_hold=not missing_start)
            folder = Path(__file__).resolve().parents[1] / "build"
            folder.mkdir(exist_ok=True)
            with tempfile.TemporaryDirectory(dir=folder) as tmp:
                with self.assertRaisesRegex(RuntimeError, "duty not observed"):
                    self.run_host(serial, clock, Path(tmp) / "trace.txt")
            self.assertEqual(serial.commands.count("STARTPULSE RR 192 2000"), 1)
            self.assertTrue(serial.closed)
            self.assertEqual(serial.control, "LOCKED")

    def test_compensated_startup_uses_full_kick_then_trim_and_resets(self):
        clock = FakeClock()
        serial = StartupSerial(clock, all_wheels=True)
        folder = Path(__file__).resolve().parents[1] / "build"
        with tempfile.TemporaryDirectory(dir=folder) as tmp:
            log = Path(tmp)/"trace.txt"
            self.run_host(serial, clock, log, compensated=True)
            text = log.read_text(encoding="utf-8")
            self.assertIn("duty=255,255,-255,-255",text)
            self.assertIn("duty=123,108,-144,-192",text)
        self.assertEqual(serial.commands.count("STARTMOVE FWD 192 2000"),1)
        self.assertFalse(serial.enabled)
        self.assertTrue(serial.closed)

    def test_unknown_trim_prevents_startup_and_resets_mode(self):
        clock = FakeClock()
        serial = StartupSerial(clock, all_wheels=True,bad_trim_profile=True)
        folder = Path(__file__).resolve().parents[1] / "build"
        with tempfile.TemporaryDirectory(dir=folder) as tmp:
            with self.assertRaisesRegex(RuntimeError,"trim profile mismatch"):
                self.run_host(serial, clock, Path(tmp)/"trace.txt",compensated=True)
        self.assertFalse(any(c.startswith("STARTMOVE ") for c in serial.commands))
        self.assertFalse(serial.enabled)
        self.assertTrue(serial.closed)

    def test_wrong_trimmed_hold_stops_without_retry(self):
        clock = FakeClock()
        serial = StartupSerial(clock,all_wheels=True,missing_hold=True)
        folder = Path(__file__).resolve().parents[1] / "build"
        with tempfile.TemporaryDirectory(dir=folder) as tmp:
            with self.assertRaisesRegex(RuntimeError,"duty not observed"):
                self.run_host(serial, clock, Path(tmp)/"trace.txt",compensated=True)
        self.assertEqual(serial.commands.count("STARTMOVE FWD 192 2000"),1)
        self.assertFalse(serial.enabled)
        self.assertTrue(serial.closed)


if __name__ == "__main__":
    unittest.main()
