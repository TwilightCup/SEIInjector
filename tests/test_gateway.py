"""Offline AU tests, including arbitrary transport read fragmentation."""
import importlib.util
import io
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("gateway", Path(__file__).resolve().parents[1] / "tools/sei_gateway.py")
gateway = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gateway)

AUD = bytes.fromhex("00000109f0")
SPS = bytes.fromhex("0000000167421122")
PPS = bytes.fromhex("00000168ce")
SEI = bytes.fromhex("000001060580")
FIRST = bytes.fromhex("0000016580aabb")  # first_mb=0
SECOND = bytes.fromhex("000000016540ccdd")  # first_mb=1
NEXT = bytes.fromhex("0000014180eeff")

class Chunks:
    def __init__(self, data, width):
        self.data, self.width, self.pos = data, width, 0
    def read(self, size):
        out = self.data[self.pos:self.pos + self.width]
        self.pos += len(out)
        return out

class GatewayTests(unittest.TestCase):
    def test_two_slices_wait_for_next_frame(self):
        self.assertIsNone(gateway._first_au_end(FIRST + SECOND))
        self.assertEqual(gateway._first_au_end(FIRST + SECOND + NEXT), len(FIRST + SECOND))

    def test_next_aud_and_parameters_stay_with_next_frame(self):
        for prefix in (AUD + SPS + PPS + SEI, SPS + PPS + SEI, SEI):
            a = AUD + FIRST + SECOND
            b = prefix + NEXT
            self.assertEqual(list(gateway.split_aus(io.BytesIO(a + b))), [a, b])

    def test_every_read_chunk_size(self):
        a, b = AUD + SPS + PPS + SEI + FIRST + SECOND, AUD + SEI + NEXT
        for width in range(1, len(a + b) + 1):
            self.assertEqual(list(gateway.split_aus(Chunks(a + b, width))), [a, b], width)

    def test_partial_next_slice_header_waits(self):
        self.assertIsNone(gateway._first_au_end(FIRST + SECOND + bytes.fromhex("00000141")))

    def test_oversize_is_not_fabricated_boundary(self):
        with self.assertRaises(ValueError):
            list(gateway.split_aus(io.BytesIO(FIRST + b'\xaa' * 100), au_cap=16))

if __name__ == "__main__":
    unittest.main(verbosity=2)
