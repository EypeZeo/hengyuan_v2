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
    def _assert_env_file_argument(self, arguments: list[str], expected: Path) -> None:
        self.assertGreaterEqual(len(arguments), 2)
        self.assertEqual(arguments[0], "--env-file")
        self.assertTrue(Path(arguments[1]).samefile(expected))

    def _powershell(self) -> str:
        if sys.platform != "win32":
            self.skipTest("the launcher contract is Windows PowerShell specific")
        pwsh = shutil.which("pwsh")
        if pwsh is None or shutil.which("python") is None:
            self.skipTest("pwsh and python must both be available on PATH")
        return pwsh

    def _prepare_launcher(self, root: Path) -> tuple[Path, Path]:
        source_script = TOOLS_DIR / "r6_allorders_visibility.ps1"
        launcher = root / source_script.name
        shutil.copyfile(source_script, launcher)
        working_directory = root / "separate working directory"
        working_directory.mkdir()
        (root / "r6_testnet_probe.py").write_text("# offline launcher placeholder\n", encoding="utf-8")
        (root / "r6_allorders_visibility.py").write_text(
            "import json, sys\n"
            "print(json.dumps({'argv': sys.argv[1:]}))\n",
            encoding="utf-8",
        )
        return launcher, working_directory

    def _invoke(self, pwsh: str, launcher: Path, working_directory: Path, args: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [pwsh, "-NoProfile", "-NonInteractive", "-File", str(launcher), *args],
            cwd=working_directory,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=20,
            check=False,
        )

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
                encoding="utf-8",
                errors="replace",
                timeout=20,
                check=False,
            )

            self.assertEqual(completed.returncode, 23, completed.stderr)
            self.assertIn(" ", str(credentials.resolve()))
            result = json.loads(completed.stdout.strip())
            self._assert_env_file_argument(result["argv"], credentials)
            self.assertEqual(
                result["argv"][2:],
                [
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
            self.assertTrue(Path(result["ssl_cert_file"]).samefile(ca_bundle))

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
                    encoding="utf-8",
                    errors="replace",
                    timeout=20,
                    check=False,
                )
                return completed, json.loads(completed.stdout.strip())

            no_order, no_order_result = invoke([])
            self.assertEqual(no_order.returncode, 0, no_order.stderr)
            self.assertNotIn("--order-id", no_order_result["argv"])
            self._assert_env_file_argument(no_order_result["argv"], credentials)
            self.assertEqual(
                no_order_result["argv"][2:],
                ["--symbol", "BTCUSDT", "--duration-seconds", "20", "--poll-interval-ms", "1000", "--timeout-seconds", "5"],
            )

            list_only, list_only_result = invoke(["-ListOnly"])
            self.assertEqual(list_only.returncode, 0, list_only.stderr)
            self.assertIn("--list-only", list_only_result["argv"])
            self.assertNotIn("--order-id", list_only_result["argv"])
            self._assert_env_file_argument(list_only_result["argv"], credentials)
            self.assertEqual(
                list_only_result["argv"][2:],
                [
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

    def test_explicit_credentials_file_takes_precedence_over_sidecar_path(self) -> None:
        pwsh = self._powershell()
        with tempfile.TemporaryDirectory(prefix="r6 visibility path ") as directory:
            root = Path(directory)
            launcher, working_directory = self._prepare_launcher(root)
            explicit = root / "explicit fake credentials.env"
            current_default = working_directory / ".env.operator"
            explicit.write_text("placeholder only\n", encoding="utf-8")
            current_default.write_text("other placeholder only\n", encoding="utf-8")
            # A broken sidecar must be ignored when the caller supplied an explicit path.
            (root / ".env.r6-visibility-path").write_text(str(root / "missing configured file") + "\n", encoding="utf-8")

            completed = self._invoke(
                pwsh,
                launcher,
                working_directory,
                ["-CredentialsFile", str(explicit.resolve()), "-ListOnly"],
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            arguments = json.loads(completed.stdout.strip())["argv"]
            self._assert_env_file_argument(arguments, explicit)
            self.assertIn("--list-only", arguments)

    def test_sidecar_credentials_path_is_resolved_from_script_directory_across_working_directories(self) -> None:
        pwsh = self._powershell()
        with tempfile.TemporaryDirectory(prefix="r6 visibility path ") as directory:
            root = Path(directory)
            launcher, working_directory = self._prepare_launcher(root)
            configured = root / "configured fake credentials.env"
            cwd_default = working_directory / ".env.operator"
            script_default = root / ".env.operator"
            configured.write_text("placeholder only\n", encoding="utf-8")
            cwd_default.write_text("wrong cwd fallback only\n", encoding="utf-8")
            script_default.write_text("wrong script fallback only\n", encoding="utf-8")
            (root / ".env.r6-visibility-path").write_text(str(configured.resolve()) + "\n", encoding="utf-8")

            completed = self._invoke(pwsh, launcher, working_directory, ["-ListOnly"])

            self.assertEqual(completed.returncode, 0, completed.stderr)
            arguments = json.loads(completed.stdout.strip())["argv"]
            self._assert_env_file_argument(arguments, configured)
            self.assertIn("--list-only", arguments)

    def test_missing_sidecar_uses_current_working_directory_default(self) -> None:
        pwsh = self._powershell()
        with tempfile.TemporaryDirectory(prefix="r6 visibility path ") as directory:
            root = Path(directory)
            launcher, working_directory = self._prepare_launcher(root)
            cwd_default = working_directory / ".env.operator"
            script_default = root / ".env.operator"
            cwd_default.write_text("current directory placeholder only\n", encoding="utf-8")
            script_default.write_text("script directory placeholder only\n", encoding="utf-8")

            completed = self._invoke(pwsh, launcher, working_directory, ["-ListOnly"])

            self.assertEqual(completed.returncode, 0, completed.stderr)
            arguments = json.loads(completed.stdout.strip())["argv"]
            self._assert_env_file_argument(arguments, cwd_default)
            self.assertIn("--list-only", arguments)

    def test_invalid_sidecar_paths_are_rejected_without_falling_back_to_cwd_credentials(self) -> None:
        pwsh = self._powershell()
        with tempfile.TemporaryDirectory(prefix="r6 visibility path ") as directory:
            root = Path(directory)
            launcher, working_directory = self._prepare_launcher(root)
            cwd_default = working_directory / ".env.operator"
            cwd_default.write_text("fallback placeholder only\n", encoding="utf-8")
            configured_directory = root / "credentials directory"
            configured_directory.mkdir()
            missing = root / "missing configured credentials.env"
            sidecar = root / ".env.r6-visibility-path"

            invalid_values = (
                str(configured_directory.resolve()) + "\n",
                str(missing.resolve()) + "\n",
                ".env.operator\n",
                str(cwd_default.resolve()) + "\n" + str(missing.resolve()) + "\n",
            )
            for value in invalid_values:
                with self.subTest(value=value):
                    sidecar.write_text(value, encoding="utf-8")

                    completed = self._invoke(pwsh, launcher, working_directory, ["-ListOnly"])

                    self.assertNotEqual(completed.returncode, 0)
                    self.assertNotIn('"argv"', completed.stdout)

    def test_missing_default_credentials_path_fails_without_parameter_prompt(self) -> None:
        pwsh = self._powershell()
        with tempfile.TemporaryDirectory(prefix="r6 visibility path ") as directory:
            root = Path(directory)
            launcher, working_directory = self._prepare_launcher(root)

            completed = self._invoke(pwsh, launcher, working_directory, [])

            self.assertNotEqual(completed.returncode, 0)
            self.assertNotIn('"argv"', completed.stdout)
            self.assertNotRegex(completed.stderr, r"(?i)supply values for the following parameters|credentialsfile:")

    def test_place_order_flags_forward_both_fake_credential_paths_and_order_fields(self) -> None:
        pwsh = self._powershell()
        with tempfile.TemporaryDirectory(prefix="r6 visibility place ") as directory:
            root = Path(directory)
            launcher, working_directory = self._prepare_launcher(root)
            read_credentials = root / "offline fake read credentials.env"
            trade_credentials = root / "offline fake trade credentials.env"
            read_credentials.write_text("placeholder only\n", encoding="utf-8")
            trade_credentials.write_text("placeholder only\n", encoding="utf-8")

            completed = self._invoke(
                pwsh,
                launcher,
                working_directory,
                [
                    "-CredentialsFile",
                    str(read_credentials.resolve()),
                    "-PlaceTestnetOrder",
                    "-TradeCredentialsFile",
                    str(trade_credentials.resolve()),
                    "-Symbol",
                    "BTCUSDT",
                    "-Side",
                    "BUY",
                    "-Price",
                    "64000.25",
                    "-Quantity",
                    "0.0025",
                ],
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            arguments = json.loads(completed.stdout.strip())["argv"]
            self._assert_env_file_argument(arguments, read_credentials)
            self.assertEqual(arguments[2], "--place-testnet-order")
            self.assertEqual(arguments[3], "--trade-env-file")
            self.assertTrue(Path(arguments[4]).samefile(trade_credentials))
            self.assertEqual(
                arguments[5:],
                [
                    "--side",
                    "BUY",
                    "--price",
                    "64000.25",
                    "--quantity",
                    "0.0025",
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
