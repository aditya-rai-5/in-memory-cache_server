#!/usr/bin/env python3
"""Self-contained configuration/startup regression tests; build the server first."""

from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest


BINARY = Path(__file__).resolve().parents[1] / "bin" / "redis_like"


class ConfigurationStartupTests(unittest.TestCase):
    def test_invalid_settings_fail_before_creating_logs(self):
        cases = [
            ("server.port=0", "server.port", "1..65535"),
            ("server.port=65536", "server.port", "1..65535"),
            ("server.port=-1", "server.port", "1..65535"),
            ("server.port=6379junk", "server.port", "invalid value"),
            ("server.port=+6379", "server.port", "invalid value"),
            ("log.max_size=-1", "log.max_size", "invalid value"),
            ("log.max_size=0", "log.max_size", "invalid value"),
            ("log.max_size=2147483648", "log.max_size", "invalid value"),
            ("log.max_files=0", "log.max_files", "1..1000"),
            ("log.max_files=1001", "log.max_files", "1..1000"),
            ("log.level=verbose", "log.level", "expected TRACE"),
            ("server.workers=4", "server.workers", "remove it"),
            ("server.potr=6379", "server.potr", "unknown setting"),
            ("log.file=", "log.file", "must not be empty"),
            ("not a setting", "expected key = value", "server.conf:2:"),
            ("=6379", "setting name", "must not be empty"),
            ("log.file=bad\0path", "NUL byte", "server.conf:2:"),
            ("server.port=6379\nserver.port=6380", "duplicate setting", ":3:"),
        ]
        for contents, *fragments in cases:
            with self.subTest(contents=contents), tempfile.TemporaryDirectory() as root:
                directory = Path(root)
                (directory / "server.conf").write_text("# test\n" + contents + "\n")
                result = subprocess.run(
                    [str(BINARY)], cwd=root, capture_output=True, text=True, timeout=5
                )
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("Configuration error: server.conf:", result.stderr)
                for fragment in fragments:
                    self.assertIn(fragment, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertFalse((directory / "logs").exists())

    def test_missing_and_unreadable_configuration(self):
        for is_directory in (False, True):
            with self.subTest(is_directory=is_directory), tempfile.TemporaryDirectory() as root:
                if is_directory:
                    (Path(root) / "server.conf").mkdir()
                result = subprocess.run(
                    [str(BINARY)], cwd=root, capture_output=True, text=True, timeout=5
                )
                self.assertEqual(result.returncode, 1)
                self.assertIn("Configuration error: server.conf:", result.stderr)
                self.assertIn(
                    "error reading" if is_directory else "cannot open", result.stderr
                )

    def test_log_directory_failure_is_reported(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            (directory / "blocked").write_text("this is a file")
            (directory / "server.conf").write_text("log.file=blocked/server.log\n")
            result = subprocess.run(
                [str(BINARY)], cwd=root, capture_output=True, text=True, timeout=5
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("Server error:", result.stderr)
            self.assertIn("blocked", result.stderr)

    def test_log_file_open_failure_is_reported(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            (directory / "log_directory").mkdir()
            (directory / "server.conf").write_text("log.file=log_directory\n")
            result = subprocess.run(
                [str(BINARY)], cwd=root, capture_output=True, text=True, timeout=5
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("Server error:", result.stderr)
            self.assertIn("log.file 'log_directory'", result.stderr)

    def test_occupied_port_is_reported(self):
        with socket.socket() as listener, tempfile.TemporaryDirectory() as root:
            listener.bind(("0.0.0.0", 0))
            listener.listen()
            port = listener.getsockname()[1]
            (Path(root) / "server.conf").write_text(f"server.port={port}\n")
            result = subprocess.run(
                [str(BINARY)], cwd=root, capture_output=True, text=True, timeout=5
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("Server error: bind() failed:", result.stderr)

    def test_configured_port_logging_and_rotation(self):
        # Reserving a port and releasing it immediately before startup avoids
        # changing the repository's configuration or depending on port 6379.
        for level, filename, backups in (("INFO", "server.log", 1),
                                          ("ERROR", "nested/server.log", 2)):
            with self.subTest(level=level), tempfile.TemporaryDirectory() as root:
                directory = Path(root)
                log_path = directory / filename
                log_path.parent.mkdir(parents=True, exist_ok=True)
                log_path.write_text("original log\n")
                Path(str(log_path) + ".1").write_text("previous backup\n")
                with socket.socket() as reservation:
                    reservation.bind(("127.0.0.1", 0))
                    port = reservation.getsockname()[1]
                (directory / "server.conf").write_text(
                    f"server.port={port}\nlog.level={level}\nlog.file={filename}\n"
                    f"log.max_size=1\nlog.max_files={backups}\n"
                )
                with (directory / "stdout").open("w+") as stdout, (
                    directory / "stderr"
                ).open("w+") as stderr:
                    process = subprocess.Popen(
                        [str(BINARY)], cwd=root, stdout=stdout, stderr=stderr
                    )
                    try:
                        deadline = time.monotonic() + 5
                        while True:
                            self.assertIsNone(process.poll(), "server exited during startup")
                            try:
                                client = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                                break
                            except ConnectionRefusedError:
                                if time.monotonic() >= deadline:
                                    self.fail("server did not listen on its configured port")
                                time.sleep(0.02)
                        with client:
                            client.settimeout(2)
                            client.sendall(struct.pack(">I", 4) + b"PING")
                            reply = b""
                            while len(reply) < 8:
                                chunk = client.recv(8 - len(reply))
                                self.assertTrue(chunk, "server closed before replying")
                                reply += chunk
                            self.assertEqual(reply, struct.pack(">I", 4) + b"PONG")
                            # An oversized frame emits ERROR and closes the client.
                            client.sendall(struct.pack(">I", 0xFFFFFFFF))
                            self.assertEqual(client.recv(1), b"")
                        while True:
                            stderr.seek(0)
                            errors = stderr.read()
                            if "Protocol error" in errors:
                                break
                            if time.monotonic() >= deadline:
                                self.fail("configured logger did not emit ERROR")
                            time.sleep(0.02)
                        stdout.seek(0)
                        messages = stdout.read()
                        self.assertEqual("Server starting on port" in messages, level == "INFO")
                        if level == "INFO":
                            self.assertIn(str(port), messages)
                        else:
                            self.assertEqual(messages, "")
                        self.assertEqual(Path(str(log_path) + ".1").read_text(), "original log\n")
                        older_backup = Path(str(log_path) + ".2")
                        self.assertEqual(older_backup.exists(), backups == 2)
                        if backups == 2:
                            self.assertEqual(older_backup.read_text(), "previous backup\n")
                        self.assertIsNone(process.poll())
                    finally:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
