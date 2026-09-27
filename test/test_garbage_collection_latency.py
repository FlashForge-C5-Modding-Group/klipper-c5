import importlib.util
from pathlib import Path
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    'garbage_collection', Path(__file__).resolve().parents[1]
    / 'klippy' / 'extras' / 'garbage_collection.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ReactorLatencyLoggingTests(unittest.TestCase):
    def test_latency_report_is_bounded_and_rate_limited(self):
        prior = MODULE._last_latency_log
        MODULE._last_latency_log = float('-inf')
        try:
            callbacks = [lambda: None] * 20
            with mock.patch.object(MODULE.logging, 'warning') as warning:
                MODULE._analyze_callback(100., 99.9, callbacks)
                MODULE._analyze_callback(100.2, 100.1, callbacks)
                self.assertEqual(warning.call_count, 1)
                self.assertIn('12 more callbacks', warning.call_args.args[-1])
                MODULE._analyze_callback(105.1, 105., callbacks)
                self.assertEqual(warning.call_count, 2)
        finally:
            MODULE._last_latency_log = prior


if __name__ == '__main__':
    unittest.main()
