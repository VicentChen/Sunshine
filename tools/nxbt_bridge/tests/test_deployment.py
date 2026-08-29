"""Static safety tests for the NXBT Bridge deployment assets."""

from pathlib import Path
import subprocess
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[3]


class DeploymentAssetTest(unittest.TestCase):
    """Verify service hardening, narrow BlueZ changes, and script reversibility."""

    def test_systemd_unit_has_bounded_restart_and_restricted_runtime(self):
        """Require Bluetooth ordering, graceful stop, and group-only socket access."""

        unit = (PROJECT_ROOT / "tools/nxbt_bridge/systemd/nxbt-bridge.service").read_text()
        for setting in (
            "Requires=bluetooth.service",
            "After=bluetooth.service",
            "StartLimitBurst=5",
            "Restart=on-failure",
            "RuntimeDirectory=nxbt-bridge",
            "RuntimeDirectoryMode=0770",
            "UMask=0007",
            "TimeoutStopSec=5",
            "KillMode=control-group",
            "Group=nxbt-bridge",
        ):
            self.assertIn(setting, unit)
        self.assertNotIn("--noplugin=*", unit)

    def test_bluez_dropin_disables_only_input_plugin(self):
        """Keep the deployment override limited to NXBT's proven conflict."""

        dropin = (PROJECT_ROOT / "tools/nxbt_bridge/systemd/bluetooth-nxbt.conf.in").read_text()
        self.assertIn("--compat --noplugin=input", dropin)
        self.assertNotIn("--noplugin=*", dropin)

    def test_install_and_uninstall_scripts_are_syntactically_valid_and_non_pairing(self):
        """Reject syntax errors and any command that changes pairings or adapter aliases."""

        scripts = [
            PROJECT_ROOT / "scripts/nxbt-bridge-preflight.sh",
            PROJECT_ROOT / "scripts/install-nxbt-bridge.sh",
            PROJECT_ROOT / "scripts/uninstall-nxbt-bridge.sh",
        ]
        subprocess.run(["bash", "-n", *map(str, scripts)], check=True)
        combined = "\n".join(script.read_text() for script in scripts)
        for forbidden in ("bluetoothctl remove", "bluetoothctl unpair", "SetAlias"):
            self.assertNotIn(forbidden, combined)
        self.assertIn('if [[ "${changed_bluez}" -eq 1 ]]', combined)
        self.assertIn("membership-added", combined)

    def test_preflight_uses_the_portable_busctl_tree_form(self):
        """Query the BlueZ tree without the unsupported optional object path."""

        preflight = (PROJECT_ROOT / "scripts/nxbt-bridge-preflight.sh").read_text()
        self.assertIn("busctl tree org.bluez 2>/dev/null", preflight)
        self.assertNotIn("busctl tree org.bluez /org/bluez", preflight)


if __name__ == "__main__":
    unittest.main()
