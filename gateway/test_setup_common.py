import json
import tempfile
import unittest
from pathlib import Path

import setup_common


class SetupCommonTests(unittest.TestCase):
    def test_tailscale_address_validation(self):
        self.assertEqual("100.64.0.1", setup_common.validate_tailscale_address("100.64.0.1"))
        self.assertEqual("100.127.255.254", setup_common.validate_tailscale_address("100.127.255.254"))
        for rejected in ("100.128.0.1", "192.168.1.5", "127.0.0.1", "not-an-ip"):
            with self.assertRaises(ValueError):
                setup_common.validate_tailscale_address(rejected)

    def test_env_uses_placeholders_owned_by_the_caller(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = setup_common.platform_paths("linux", Path(directory))
            text = setup_common.env_text(
                "100.64.10.20", paths, "generated-test-token", Path(directory) / "openclaw.json"
            )
            self.assertIn("CREW_CHIEF_BIND_HOST=100.64.10.20", text)
            self.assertIn("OPENCLAW_MODEL=openclaw/racebox-crew-chief", text)
            self.assertIn("CREW_CHIEF_CLIENT_TOKEN=generated-test-token", text)
            self.assertNotIn("OPENCLAW_AUTH_TOKEN", text)

    def test_openclaw_patch_is_agent_scoped_and_minimal(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "openclaw.json"
            path.write_text(
                json.dumps({"agents": {"list": [{"id": setup_common.AGENT_ID}], "defaults": {}}}),
                encoding="utf-8",
            )
            workspace = Path(directory) / "workspace"
            setup_common.configure_openclaw(path, workspace, "provider/example-model")
            configured = json.loads(path.read_text(encoding="utf-8"))
            agent = configured["agents"]["list"][0]
            self.assertEqual("minimal", agent["tools"]["profile"])
            self.assertIn("group:runtime", agent["tools"]["deny"])
            self.assertEqual(["racebox-vehicle-dynamics"], agent["skills"])
            self.assertEqual("xhigh", agent["thinkingDefault"])
            self.assertEqual("provider/example-model", agent["model"])

    def test_rollback_restores_replaced_and_removes_created_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            existing = root / "existing.txt"
            existing.write_text("before", encoding="utf-8")
            created = root / "created.txt"
            manifest = {"backup_root": str(root / "backup"), "replaced": [], "created": []}
            replacement = root / "replacement.txt"
            replacement.write_text("after", encoding="utf-8")
            setup_common.copy_with_backup(replacement, existing, manifest)
            setup_common.copy_with_backup(replacement, created, manifest)
            manifest_path = root / "rollback.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            setup_common.rollback(manifest_path)
            self.assertEqual("before", existing.read_text(encoding="utf-8"))
            self.assertFalse(created.exists())


if __name__ == "__main__":
    unittest.main()
