import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
from motion_sequence import check_ramp_trace, wheel_duties


class RampTraceTests(unittest.TestCase):
    def trace(self, levels, sign=1):
        return [[sign*v, sign*v, -sign*v, -sign*v] for v in levels]

    def test_forward_and_backward(self):
        for sign in (1, -1):
            self.assertEqual(check_ramp_trace(self.trace([8, 48, 96, 144, 190, 144, 96, 48, 8], sign),
                                             (sign,)*4, 192), 190)

    def test_reject_missing_deceleration(self):
        with self.assertRaises(RuntimeError):
            check_ramp_trace(self.trace([8, 48, 96, 144, 190, 192]), (1,)*4, 192)

    def test_reject_sign_or_wheel_mismatch(self):
        for bad in ([8, 8, 8, -8], [8, 9, -8, -8], [193, 193, -193, -193]):
            with self.assertRaises(RuntimeError):
                check_ramp_trace([bad], (1,)*4, 192)

    def test_reject_nonmonotonic_or_low_peak(self):
        for levels in ([8, 80, 48, 190, 96, 8], [8, 48, 96, 80, 48, 8],
                       [8, 96, 190, 96, 144, 8]):
            with self.assertRaises(RuntimeError):
                check_ramp_trace(self.trace(levels), (1,)*4, 192)

    def test_compensated_forward_backward_and_bounds(self):
        self.assertEqual(wheel_duties((1,)*4, 192, True), [123, 108, -144, -192])
        for sign in (1, -1):
            logical = (sign,)*4
            trace = [wheel_duties(logical, v, True) for v in [8, 48, 96, 144, 190, 144, 96, 48, 8]]
            self.assertEqual(check_ramp_trace(trace, logical, 192, True), 190)
        for level in range(193):
            sample = wheel_duties((1,)*4, level, True)
            self.assertLessEqual(max(map(abs, sample)), 192)
            self.assertEqual(wheel_duties((-1,)*4, level, True), [-v for v in sample])
        self.assertEqual(wheel_duties((0, 1, 1, 0), 192, True), [0, 108, -144, 0])

    def test_compensated_trace_rejects_raw_and_incorrect_trim(self):
        with self.assertRaises(RuntimeError):
            check_ramp_trace(self.trace([8, 48, 96, 144, 190, 144, 96, 48, 8]), (1,)*4, 192, True)
        trace = [wheel_duties((1,)*4, v, True) for v in [8, 48, 96, 144, 190, 144, 96, 48, 8]]
        trace[4][3] += 1
        with self.assertRaises(RuntimeError):
            check_ramp_trace(trace, (1,)*4, 192, True)


if __name__ == "__main__":
    unittest.main()
