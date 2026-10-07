"""Validate the real-input route adapter without launching a game."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("route_adapter", Path(__file__).resolve().parents[2] /
                                             "tools" / "prepare_benchmark_route.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class RouteAdapterTest(unittest.TestCase):
    def test_preserves_guest_inputs_and_boundaries(self):
        text, frames = adapter.convert_route([
            {"frames": 300, "pad": 0, "label": "menu"},
            {"frames": 10, "pad": 4096}, {"frames": 90, "pad": 65535}], 400)
        self.assertEqual((text, frames), ("300 0\n10 4096\n90 65535\n", 400))

    def test_rejects_wrong_shape(self):
        for value in ([], {}, None, [1], [{"frames": 1}]):
            with self.subTest(value=value), self.assertRaises(ValueError):
                adapter.convert_route(value)

    def test_rejects_invalid_numbers_and_bool(self):
        for frames, pad in ((0, 0), (-1, 0), (True, 0), (1.5, 0),
                            (1, -1), (1, 65536), (1, False), (1, "4")):
            with self.subTest(frames=frames, pad=pad), self.assertRaises(ValueError):
                adapter.convert_route([{"frames": frames, "pad": pad}])

    def test_rejects_overflow_and_wrong_expected_boundary(self):
        with self.assertRaises(ValueError):
            adapter.convert_route([{"frames": 1000000, "pad": 0}, {"frames": 1, "pad": 0}])
        with self.assertRaises(ValueError):
            adapter.convert_route([{"frames": 10, "pad": 0}], 11)


if __name__ == "__main__":
    unittest.main()
