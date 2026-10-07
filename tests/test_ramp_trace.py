import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
from motion_sequence import check_ramp_trace


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


if __name__ == "__main__":
    unittest.main()
