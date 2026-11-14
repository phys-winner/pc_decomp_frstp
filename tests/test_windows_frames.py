"""Small offline checks for PresentMon CSV validation and cadence arithmetic."""
import csv
import io
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_windows_frames import analyze

HEADER = ['Application', 'ProcessID', 'SwapChainAddress', 'Runtime', 'Dropped', 'TimeInSeconds', 'msBetweenPresents']


def document(times, change=None):
    stream = io.StringIO(newline='')
    writer = csv.writer(stream)
    writer.writerow(HEADER)
    for index, timestamp in enumerate(times):
        row = dict(zip(HEADER, ['Frostpunk.exe', '123', '0x1234', 'DXGI', '0', str(timestamp), '999']))
        if change:
            change(index, row)
        writer.writerow([row[key] for key in HEADER])
    return stream.getvalue()


class WindowsFramesTest(unittest.TestCase):
    def test_actual_timestamp_intervals_and_dropped_rows(self):
        report = analyze(document([0.01, 0.02, 0.04], lambda i, r: r.update(Dropped=str(int(i == 1)))), 123)
        self.assertAlmostEqual(report['average_fps'], 2 / 0.03)
        self.assertAlmostEqual(report['capture_duration_s'], 0.03)
        self.assertAlmostEqual(report['average_frame_ms'], 15)
        self.assertAlmostEqual(report['p99_frame_ms'], 20)
        self.assertEqual((report['frames'], report['intervals'], report['dropped_count'], report['rows_filtered']), (3, 2, 1, 0))
        self.assertIsNone(report['slowest_1_percent_average_fps'])
        self.assertFalse(report['display_cadence_computed'])

    def test_slowest_one_percent_uses_mean_frame_time(self):
        times = [i / 100 for i in range(199)] + [2.0, 2.04]
        report = analyze(document(times), 123)
        self.assertEqual(report['intervals'], 200)
        self.assertEqual(report['slowest_1_percent_interval_count'], 2)
        self.assertAlmostEqual(report['slowest_1_percent_average_fps'], 1000 / 30)
        self.assertAlmostEqual(report['p99_frame_ms'], 10)
        self.assertAlmostEqual(report['average_fps'], 200 / 2.04)

    def test_bad_rows_are_rejected(self):
        for field, value in [('ProcessID', '124'), ('ProcessID', '123.0'), ('Runtime', 'D3D9'),
                             ('SwapChainAddress', '0x5678'), ('SwapChainAddress', '0'),
                             ('Application', 'other.exe'), ('Dropped', '2'), ('Dropped', ''),
                             ('TimeInSeconds', 'nan'), ('TimeInSeconds', 'inf'),
                             ('TimeInSeconds', '-1'), ('TimeInSeconds', '0'), ('TimeInSeconds', 'bad')]:
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                analyze(document([0, 0.01], lambda i, r: r.update({field: value}) if i else None), 123)

    def test_schema_empty_duplicate_time_and_blank_rows(self):
        for text in ['', document([]), document([0]), document([0, 0]), document([0.02, 0.01]),
                     document([0, 0.01]) + '\n', document([0, 0.01]) + 'broken,row\n',
                     document([0, 0.01]).replace('ProcessID', 'Other'),
                     document([0, 0.01]).replace('msBetweenPresents', 'TimeInSeconds')]:
            with self.subTest(text=text), self.assertRaises(ValueError):
                analyze(text, 123)
        for pid in [0, -1, True]:
            with self.subTest(pid=pid), self.assertRaises(ValueError):
                analyze(document([0, 0.01]), pid)


if __name__ == '__main__':
    unittest.main()
