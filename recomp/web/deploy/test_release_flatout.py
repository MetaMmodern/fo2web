import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("release_flatout", Path(__file__).with_name("release_flatout.py"))
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.commit = subprocess.check_output(["git", "-C", str(r.ROOT), "rev-parse", "HEAD"], text=True).strip()
        (self.path / "nfsu2.wasm").write_bytes(b"\x00asm\x01\x00\x00\x00")
        self.write_meta("flatout2-" + self.commit[:7], 8)
        self.env = {"FLATOUT_IMAGE": "ghcr.io/example/flatout-rs", "COOLIFY_URL": "https://coolify.example",
                    "COOLIFY_TOKEN": "secret", "COOLIFY_APP_UUID": "abc123"}

    def write_meta(self, rev, size):
        (self.path / "nfsu2.json").write_text(json.dumps({"rev": rev, "size": size}))

    def test_validation(self):
        self.assertEqual(r.validate_module(self.path)[0], self.commit)
        self.write_meta("flatout2-" + self.commit[:7] + "+", 8)
        with self.assertRaises(ValueError):
            r.validate_module(self.path)
        self.write_meta("flatout2-" + self.commit[:7], 9)
        with self.assertRaises(ValueError):
            r.validate_module(self.path)

    def test_upload_failure_never_changes_coolify(self):
        calls = []
        def api(base, token, method, path, payload=None):
            calls.append(method)
            return ([{"key": "NFSU2_WASM", "is_preview": False}] if path.endswith("/envs") else {})
        original_run = r.run
        def run(args, **kw):
            if args[0] == "scp":
                raise subprocess.CalledProcessError(1, args)
            return original_run(args, **kw)
        with patch.dict(os.environ, self.env), patch.object(sys, "argv", ["release", "user@host", str(self.path)]), \
             patch.object(r, "api", side_effect=api), patch.object(r, "ssh", side_effect=[self.commit, ""]), \
             patch.object(r, "run", side_effect=run):
            with self.assertRaises(subprocess.CalledProcessError):
                r.main()
        self.assertEqual(calls, ["GET", "GET"])

    def test_mismatched_image_never_changes_coolify(self):
        calls = []
        def api(base, token, method, path, payload=None):
            calls.append(method)
            return ([{"key": "NFSU2_WASM"}] if path.endswith("/envs") else {})
        with patch.dict(os.environ, self.env), patch.object(sys, "argv", ["release", "user@host", str(self.path)]), \
             patch.object(r, "api", side_effect=api), patch.object(r, "ssh", return_value="wrong"):
            with self.assertRaises(ValueError):
                r.main()
        self.assertEqual(calls, ["GET", "GET"])

    def test_deploy_only_after_verified_upload(self):
        events = []
        def api(base, token, method, path, payload=None):
            events.append((method, path))
            if path.endswith("/envs") and method == "GET":
                return [{"key": "NFSU2_WASM"}]
            if path.startswith("/deploy?"):
                return {"deployments": [{"deployment_uuid": "deploy123"}]}
            if path == "/deployments/deploy123":
                return {"status": "finished"}
            return {}
        def ssh(host, script, *args):
            events.append(("SSH", args))
            return self.commit if "docker pull" in script else ""
        original_run = r.run
        def run(args, **kw):
            if args[0] == "scp":
                events.append(("SCP", args[-1]))
                return None
            return original_run(args, **kw)
        with patch.dict(os.environ, self.env), patch.object(sys, "argv", ["release", "user@host", str(self.path)]), \
             patch.object(r, "api", side_effect=api), patch.object(r, "ssh", side_effect=ssh), \
             patch.object(r, "run", side_effect=run):
            r.main()
        kinds = [event[0] for event in events]
        self.assertEqual(kinds, ["GET", "GET", "SSH", "SSH", "SCP", "SCP", "SCP", "SSH", "PATCH", "PATCH", "POST", "GET"])

    def test_dev_requires_explicit_full_commit_and_default_rejects_override(self):
        self.write_meta("flatout2-2fee04b+", 8)
        for flags in ([], ["--dev-release"], ["--dev-release", "--image-commit", "abc1234"],
                      ["--image-commit", self.commit]):
            with self.subTest(flags=flags), patch.dict(os.environ, self.env), \
                 patch.object(sys, "argv", ["release", "-n", *flags, "user@host", str(self.path)]), \
                 patch.object(r, "api") as api, patch.object(r, "ssh") as ssh:
                with self.assertRaises(ValueError):
                    r.main()
                api.assert_not_called()
                ssh.assert_not_called()

    def test_dev_record_and_original_metadata(self):
        self.write_meta("flatout2-2fee04b+", 8)
        original = (self.path / "nfsu2.json").read_bytes()
        records, events = [], []
        def api(base, token, method, path, payload=None):
            events.append(method)
            if method == "GET" and path.endswith("/envs"):
                return [{"key": "NFSU2_WASM"}]
            if path.startswith("/deploy?"):
                return {"deployments": [{"deployment_uuid": "deploy123"}]}
            if path.startswith("/deployments/"):
                return {"status": "finished"}
            return {}
        def ssh(host, script, *args):
            events.append("verify" if "sha256sum" in script else "ssh")
            if "sha256sum" in script:
                self.assertIn("release.json", script)
                self.assertEqual(args[-1], r.hashlib.sha256(records[0]).hexdigest())
            return self.commit if "docker pull" in script else ""
        def run(args, **kwargs):
            if args[0] == "scp":
                if args[2].endswith("release.json"):
                    records.append(Path(args[2]).read_bytes())
                return None
            return original_run(args, **kwargs)
        original_run = r.run
        with patch.dict(os.environ, self.env), \
             patch.object(sys, "argv", ["release", "--dev-release", "--image-commit", self.commit, "user@host", str(self.path)]), \
             patch.object(r, "api", side_effect=api), patch.object(r, "ssh", side_effect=ssh), \
             patch.object(r, "run", side_effect=run):
            r.main()
        record = json.loads(records[0])
        self.assertEqual(record["verification"], "unverified")
        self.assertEqual(record["wasm_revision_claim"], "flatout2-2fee04b+")
        self.assertEqual(record["image_commit"], self.commit)
        self.assertEqual(record["wasm_sha256"], r.sha256(self.path / "nfsu2.wasm"))
        self.assertEqual((self.path / "nfsu2.json").read_bytes(), original)
        self.assertLess(events.index("verify"), events.index("PATCH"))


if __name__ == "__main__":
    unittest.main()
