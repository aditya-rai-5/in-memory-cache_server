#!/usr/bin/env python3
"""Bounded subprocess tests for JSON, rotation, queue policies, and fatal logging."""

import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest


BINARY = Path(os.environ.get(
    "LOGGER_TEST_BINARY", Path(__file__).resolve().parents[1] / "bin/test_logger"
)).resolve()


class LoggerTests(unittest.TestCase):
    def run_case(self, name, fatal=False):
        workspace = tempfile.TemporaryDirectory(prefix="cache-logger-")
        self.addCleanup(workspace.cleanup)
        root = Path(workspace.name)
        result = subprocess.run(
            [str(BINARY), name, str(root)], capture_output=True, text=True,
            encoding="utf-8", timeout=15,
        )
        self.assertEqual(result.returncode, -signal.SIGABRT if fatal else 0,
                         result.stdout + "\n" + result.stderr)
        return root, result

    def records(self, root):
        base = root / "server.log"
        archives = sorted(
            (path for path in root.glob("server.log.*") if path.is_file()),
            key=lambda path: int(path.suffix[1:]), reverse=True,
        )
        records = []
        for path in archives + ([base] if base.is_file() else []):
            data = path.read_bytes()
            if data:
                self.assertTrue(data.endswith(b"\n"), f"partial final record: {path}")
            for line in data.decode("utf-8").splitlines():
                record = json.loads(line)
                self.assertEqual(set(record), {"ts", "level", "file", "line", "thread", "msg"})
                self.assertIsInstance(record["thread"], str)
                self.assertIsInstance(record["line"], int)
                records.append(record)
        return records

    def test_json_controls_unicode_and_metadata(self):
        root, result = self.run_case("controls")
        records = self.records(root)
        self.assertEqual(len(records), 8)
        self.assertEqual(records[0]["msg"], "controls=" + "".join(map(chr, range(32))) + '"\\ café雪😀')
        self.assertEqual(records[0]["file"], 'source"\n\t\\\x01.cpp')
        self.assertEqual(records[1]["msg"], "")
        invalid = (b"\xff", b"\xc0\xaf", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe2\x82", b"\x80")
        self.assertEqual([record["msg"] for record in records[2:]],
                         [value.decode("latin-1") for value in invalid])
        self.assertEqual([json.loads(line) for line in result.stdout.splitlines()], records)

    def test_rotation_counts_buffered_records(self):
        root, _ = self.run_case("rotation")
        records = self.records(root)
        self.assertEqual([record["msg"] for record in records], [f"record_{i}" for i in range(8)])
        self.assertTrue((root / "server.log.1").exists())
        self.assertTrue((root / "server.log.2").exists())
        for path in root.glob("server.log*"):
            # Rotate at the next record boundary after reaching 256 bytes.
            # A complete record can cross the threshold; it is never split.
            self.assertLess(path.stat().st_size, 256 + 256)

    def test_short_interrupted_writes_and_large_record(self):
        root, _ = self.run_case("short_writes")
        records = self.records(root)
        self.assertEqual([record["msg"] for record in records], ["x" * 20000, "after_large_record"])

    def test_blocking_queue_notifies_when_space_opens(self):
        root, _ = self.run_case("blocking")
        self.assertEqual([record["msg"] for record in self.records(root)], ["first", "queued", "blocked"])

    def test_drop_policy_preserves_already_accepted_records(self):
        root, _ = self.run_case("drop")
        self.assertEqual([record["msg"] for record in self.records(root)], ["first", "queued"])

    def test_shutdown_releases_waiters_and_serializes_join(self):
        root, _ = self.run_case("shutdown")
        self.assertEqual([record["msg"] for record in self.records(root)], ["first", "queued"])

    def test_failed_initialization_rolls_back_descriptors(self):
        root, _ = self.run_case("init_failures")
        self.assertEqual([record["msg"] for record in self.records(root)], ["recovered"])

    def test_reinitialization_cloexec_and_close_eintr(self):
        root, result = self.run_case("lifecycle")
        self.assertEqual([record["msg"] for record in self.records(root)], [f"session_{i}" for i in range(20)])
        self.assertIn("cannot close log.file", result.stderr)

    def test_sink_failures_wake_waiters_without_terminating_process(self):
        for name in ("write_failure", "zero_write", "rotation_failure", "rotation_open_failure"):
            with self.subTest(name=name):
                root, result = self.run_case(name)
                self.assertIn("Logging error:", result.stderr)
                if name.startswith("rotation"):
                    self.assertEqual(self.records(root)[0]["msg"], "first")

    def test_concurrent_producers_shutdown_and_reinitialization(self):
        root, _ = self.run_case("concurrency")
        records = self.records(root)
        self.assertTrue(records)
        self.assertEqual(records[-1]["msg"], "final_session")
        messages = [record["msg"] for record in records]
        self.assertEqual(len(messages), len(set(messages)), "no duplicate or corrupted records")

    def test_fatal_drains_full_queue_and_syncs_current_inode(self):
        for policy in ("drop", "block"):
            with self.subTest(policy=policy):
                root, _ = self.run_case("fatal_" + policy, fatal=True)
                self.assertEqual([record["msg"] for record in self.records(root)],
                                 ["first", "queued", "fatal_marker\n\t\x01"])
                self.assertEqual((root / "sync.txt").read_text(), "current inode\nfatal flushed\n")

    def test_fatal_without_active_logger_still_reports_and_aborts(self):
        for name in ("fatal_uninitialized", "fatal_after_shutdown"):
            with self.subTest(name=name):
                root, result = self.run_case(name, fatal=True)
                record = json.loads(result.stderr.strip())
                self.assertEqual(record["level"], "FATAL")
                self.assertIn("fatal_marker", record["msg"])
                if name == "fatal_after_shutdown":
                    self.assertEqual(self.records(root)[0]["msg"], "before_shutdown")

    def test_fatal_serializes_with_shutdown_already_in_progress(self):
        root, result = self.run_case("fatal_during_shutdown", fatal=True)
        self.assertEqual([record["msg"] for record in self.records(root)], ["first", "queued"])
        reports = [json.loads(line) for line in result.stderr.splitlines() if line.startswith("{")]
        self.assertEqual(reports[-1]["level"], "FATAL")
        self.assertIn("fatal_marker", reports[-1]["msg"])

    def test_fatal_sink_or_sync_failure_reports_and_aborts(self):
        for name in ("fatal_write_failure", "fatal_sync_failure"):
            with self.subTest(name=name):
                _, result = self.run_case(name, fatal=True)
                self.assertIn("Logging error:", result.stderr)
                reports = [json.loads(line) for line in result.stderr.splitlines() if line.startswith("{")]
                self.assertTrue(any(record["level"] == "FATAL" and "fatal_marker" in record["msg"]
                                    for record in reports))


if __name__ == "__main__":
    unittest.main(verbosity=2)
