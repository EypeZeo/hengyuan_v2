"""Offline PowerShell argument-forwarding test for the visibility launcher."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]


class WindowsLauncherTests(unittest.TestCase):
    def test_launcher_preserves_absolute_paths_with_spaces_and_forwards_exit_code(self) -> None:
        if sys.platform != "win32":
            self.skipTest("the launcher contract is Windows PowerShell specific")
        pwsh = shutil.which("pwsh")
        if pwsh is None or shutil.which("python") is None:
            self.skipTest("pwsh and python must both be available on PATH")

        source_script = TOOLS_DIR / "r6_allorders_visibility.ps1"
        with tempfile.TemporaryDirectory(prefix="r6 visibility launcher ") as directory:
            root = Path(directory)
            working_directory = root / "separate working directory"
            working_directory.mkdir()
            launcher = root / source_script.name
            shutil.copyfile(source_script, launcher)
            credentials = root / "offline fake credentials.env"
            ca_bundle = root / "offline fake CA bundle.pem"
            credentials.write_text("placeholder only\n", encoding="utf-8")
            ca_bundle.write_text("placeholder only\n", encoding="utf-8")
            (root / "r6_testnet_probe.py").write_text("# offline launcher placeholder\n", encoding="utf-8")
            (root / "r6_allorders_visibility.py").write_text(
                "import json, os, sys\n"
                "print(json.dumps({'argv': sys.argv[1:], 'ssl_cert_file': os.environ.get('SSL_CERT_FILE')}))\n"
                "raise SystemExit(int(os.environ.get('FAKE_SAMPLER_EXIT', '0')))\n",
                encoding="utf-8",
            )
            environment = os.environ.copy()
            environment["FAKE_SAMPLER_EXIT"] = "23"
            command = [
                pwsh,
                "-NoProfile",
                "-File",
                str(launcher),
                "-CredentialsFile",
                str(credentials.resolve()),
                "-OrderId",
                "9223372036854775807",
                "-Symbol",
                "ETHUSDT",
                "-DurationSeconds",
                "7",
                "-PollIntervalMs",
                "750",
                "-TimeoutSeconds",
                "9",
                "-CaBundle",
                str(ca_bundle.resolve()),
            ]
            completed = subprocess.run(
                command,
                cwd=working_directory,
                env=environment,
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )

            self.assertEqual(completed.returncode, 23, completed.stderr)
            self.assertIn(" ", str(credentials.resolve()))
            result = json.loads(completed.stdout.strip())
            self.assertEqual(
                result["argv"],
                [
                    "--env-file",
                    str(credentials.resolve()),
                    "--order-id",
                    "9223372036854775807",
                    "--symbol",
                    "ETHUSDT",
                    "--duration-seconds",
                    "7",
                    "--poll-interval-ms",
                    "750",
                    "--timeout-seconds",
                    "9",
                ],
            )
            self.assertEqual(result["ssl_cert_file"], str(ca_bundle.resolve()))

    def test_launcher_omits_unspecified_order_id_and_forwards_list_only(self) -> None:
        if sys.platform != "win32":
            self.skipTest("the launcher contract is Windows PowerShell specific")
        pwsh = shutil.which("pwsh")
        if pwsh is None or shutil.which("python") is None:
            self.skipTest("pwsh and python must both be available on PATH")

        source_script = TOOLS_DIR / "r6_allorders_visibility.ps1"
        with tempfile.TemporaryDirectory(prefix="r6 visibility launcher ") as directory:
            root = Path(directory)
            working_directory = root / "separate working directory"
            working_directory.mkdir()
            launcher = root / source_script.name
            shutil.copyfile(source_script, launcher)
            credentials = root / "offline fake credentials.env"
            credentials.write_text("placeholder only\n", encoding="utf-8")
            (root / "r6_testnet_probe.py").write_text("# offline launcher placeholder\n", encoding="utf-8")
            (root / "r6_allorders_visibility.py").write_text(
                "import json, sys\n"
                "print(json.dumps({'argv': sys.argv[1:]}))\n",
                encoding="utf-8",
            )

            def invoke(extra_arguments: list[str]) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
                completed = subprocess.run(
                    [pwsh, "-NoProfile", "-File", str(launcher), "-CredentialsFile", str(credentials.resolve()), *extra_arguments],
                    cwd=working_directory,
                    capture_output=True,
                    text=True,
                    timeout=20,
                    check=False,
                )
                return completed, json.loads(completed.stdout.strip())

            no_order, no_order_result = invoke([])
            self.assertEqual(no_order.returncode, 0, no_order.stderr)
            self.assertNotIn("--order-id", no_order_result["argv"])
            self.assertEqual(
                no_order_result["argv"],
                [
                    "--env-file",
                    str(credentials.resolve()),
                    "--symbol",
                    "BTCUSDT",
                    "--duration-seconds",
                    "20",
                    "--poll-interval-ms",
                    "1000",
                    "--timeout-seconds",
                    "5",
                ],
            )

            list_only, list_only_result = invoke(["-ListOnly"])
            self.assertEqual(list_only.returncode, 0, list_only.stderr)
            self.assertIn("--list-only", list_only_result["argv"])
            self.assertNotIn("--order-id", list_only_result["argv"])
            self.assertEqual(
                list_only_result["argv"],
                [
                    "--env-file",
                    str(credentials.resolve()),
                    "--list-only",
                    "--symbol",
                    "BTCUSDT",
                    "--duration-seconds",
                    "20",
                    "--poll-interval-ms",
                    "1000",
                    "--timeout-seconds",
                    "5",
                ],
            )


if __name__ == "__main__":
    unittest.main()
