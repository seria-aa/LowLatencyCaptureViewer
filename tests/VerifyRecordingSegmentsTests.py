"""The recovery checker may exclude only the final uninitialized crash file."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import sys
sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location("segments", Path(__file__).with_name("Verify-RecordingSegments.py"))
segments = importlib.util.module_from_spec(spec)
spec.loader.exec_module(segments)


class RecoveryPolicy(unittest.TestCase):
    def test_tail_policy(self):
        with tempfile.TemporaryDirectory(prefix="LLCV-RecoveryPolicy-") as directory:
            root = Path(directory)
            good = root / "complete-001.mp4"
            tail = root / "complete-002.mp4"
            ftyp = (16).to_bytes(4, "big") + b"ftyp" + b"isom" + bytes(4)
            moov = (8).to_bytes(4, "big") + b"moov"
            good.write_bytes(ftyp + moov)
            third = root / "complete-003.mp4"
            third.write_bytes(ftyp + moov)
            self.assertTrue(segments.initialized(good))
            for content in (b"", ftyp[:8], ftyp, ftyp + (64).to_bytes(4, "big") + b"moov",
                            ftyp + b"\x00\x00\x00\x01moov",
                            ftyp + (8).to_bytes(4, "big") + b"free"):
                tail.write_bytes(content)
                self.assertFalse(segments.initialized(tail))
                self.assertEqual(segments.recoverable_segments([tail, good], True), ([good], tail.name))
                with self.assertRaises(AssertionError):
                    segments.recoverable_segments([good, tail], False)
                with self.assertRaises(AssertionError):
                    segments.recoverable_segments([tail, good, third], True)
            for content in (b"\x00\x00", b"garbage!", ftyp + b"garbage!",
                            ftyp + (64).to_bytes(4, "big") + b"mdat",
                            ftyp + (8).to_bytes(4, "big") + b"moof",
                            ftyp + (4).to_bytes(4, "big") + b"moov",
                            ftyp + bytes(4) + b"moov",
                            b"\x00\x00\x00\x01moov"):
                tail.write_bytes(content)
                self.assertEqual(segments.initialization_state(tail), "invalid")
                with self.assertRaises(AssertionError):
                    segments.recoverable_segments([good, tail], True)
            tail.write_bytes(ftyp + moov + b"corrupt fragment")
            self.assertEqual(segments.recoverable_segments([good, tail], True), ([good, tail], None))
            # Extended box sizes and truncated extended headers.
            tail.write_bytes(b"\x00\x00\x00\x01moov" + (16).to_bytes(8, "big"))
            self.assertTrue(segments.initialized(tail))
            tail.write_bytes(b"\x00\x00\x00\x01moov")
            self.assertFalse(segments.initialized(tail))


if __name__ == "__main__":
    unittest.main()
