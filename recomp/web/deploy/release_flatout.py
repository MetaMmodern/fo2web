#!/usr/bin/env python3
"""Stage a locally built FlatOut module and explicitly deploy its matching image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[3]
REMOTE_ROOT = "/srv/flatout/module/releases"


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def validate_module(folder, dev_release=False):
    wasm, meta = folder / "nfsu2.wasm", folder / "nfsu2.json"
    info = json.loads(meta.read_text())
    rev = info.get("rev", "")
    if not isinstance(rev, str) or not re.fullmatch(r"flatout2-[0-9a-f]{7,40}\+?" if dev_release else r"flatout2-[0-9a-f]{7,40}", rev):
        raise ValueError(f"expected a clean flatout2-<commit> revision, got {rev!r}")
    if info.get("size") != wasm.stat().st_size:
        raise ValueError("WASM size does not match nfsu2.json")
    with wasm.open("rb") as f:
        if f.read(8) != b"\x00asm\x01\x00\x00\x00":
            raise ValueError("not a WebAssembly version 1 module")
    commit = None if dev_release else run(["git", "-C", str(ROOT), "rev-parse", "--verify", rev[9:] + "^{commit}"],
                                          capture_output=True).stdout.strip()
    return commit, sha256(wasm), sha256(meta)


def api(base, token, method, path, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(base + "/api/v1" + path, data=data, method=method,
                                 headers={"Authorization": "Bearer " + token,
                                          "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as response:
        body = response.read()
    return json.loads(body) if body else {}


def ssh(host, script, *args):
    cmd = "bash -s -- " + " ".join(shlex.quote(arg) for arg in args)
    return run(["ssh", "--", host, cmd], input=script, capture_output=True).stdout.strip()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dry-run", "-n", action="store_true", help="validate the local module only")
    p.add_argument("--dev-release", action="store_true", help="explicitly permit an unverified module/image combination")
    p.add_argument("--image-commit", help="full server image commit (required only with --dev-release)")
    p.add_argument("vps", help="SSH host, e.g. deploy@example.com")
    p.add_argument("module", type=Path, help="folder containing nfsu2.wasm and nfsu2.json")
    a = p.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.@-]+", a.vps) or a.vps.startswith("-"):
        raise ValueError("invalid SSH host")
    if a.dev_release:
        if not a.image_commit or not re.fullmatch(r"[0-9a-f]{40}", a.image_commit):
            raise ValueError("--dev-release requires --image-commit with a full 40-character lowercase commit")
    elif a.image_commit is not None:
        raise ValueError("--image-commit requires --dev-release")
    commit, wasm_hash, json_hash = validate_module(a.module, a.dev_release)
    commit = a.image_commit if a.dev_release else commit
    metadata = (a.module / "nfsu2.json").read_bytes()
    if hashlib.sha256(metadata).hexdigest() != json_hash:
        raise ValueError("WASM metadata changed during validation")
    revision = json.loads(metadata)["rev"]
    record = {"verification": "unverified" if a.dev_release else "revision-matched",
              "wasm_revision_claim": revision, "wasm_sha256": wasm_hash,
              "metadata_sha256": json_hash, "image_commit": commit,
              "note": "WASM revision is a metadata claim, not proof of build provenance."}
    record_bytes = (json.dumps(record, sort_keys=True, indent=2) + "\n").encode()
    record_hash = hashlib.sha256(record_bytes).hexdigest()
    repo = os.environ.get("FLATOUT_IMAGE", "")
    if not re.fullmatch(r"ghcr\.io/[a-z0-9_.-]+/[a-z0-9_.-]+", repo):
        raise ValueError("set FLATOUT_IMAGE to the lowercase GHCR image name, without a tag")
    image = f"{repo}:sha-{commit}"
    release_id = f"{commit}-{wasm_hash[:16]}-{json_hash[:16]}-{record_hash[:16]}"
    print(f"status: {record['verification']}\nWASM revision claim: {revision}\nWASM SHA-256: {wasm_hash}\nimage: {image}\nrelease: {release_id}", flush=True)
    if a.dry_run:
        return
    base = os.environ.get("COOLIFY_URL", "").rstrip("/")
    token = os.environ.get("COOLIFY_TOKEN", "")
    app = os.environ.get("COOLIFY_APP_UUID", "")
    if not base.startswith("https://") or not token or not re.fullmatch(r"[A-Za-z0-9-]+", app):
        raise ValueError("set HTTPS COOLIFY_URL, COOLIFY_TOKEN and COOLIFY_APP_UUID")
    prefix = "/applications/" + app
    current = api(base, token, "GET", prefix)
    envs = api(base, token, "GET", prefix + "/envs")
    if not isinstance(current, dict) or not any(e.get("key") == "NFSU2_WASM" and not e.get("is_preview") for e in envs):
        raise ValueError("Coolify application must already have a runtime NFSU2_WASM variable")
    # Verify the exact image on the VPS, using that host's Docker registry credentials.
    label = ssh(a.vps, 'set -euo pipefail\ndocker pull "$1" >&2\ndocker image inspect --format \'{{index .Config.Labels "org.opencontainers.image.revision"}}\' "$1"\n', image)
    if label != commit:
        raise ValueError(f"image revision label {label!r} does not match {commit}")
    staging = f"{REMOTE_ROOT}/.incoming-{uuid.uuid4().hex}"
    final = f"{REMOTE_ROOT}/{release_id}"
    ssh(a.vps, 'set -euo pipefail\nmkdir -p "$1"\nmkdir -m 755 "$2"\n', REMOTE_ROOT, staging)
    for name in ("nfsu2.wasm", "nfsu2.json"):
        run(["scp", "--", str(a.module / name), f"{a.vps}:{staging}/{name}"])
    with tempfile.TemporaryDirectory() as tmp:
        record_path = Path(tmp) / "release.json"
        record_path.write_bytes(record_bytes)
        run(["scp", "--", str(record_path), f"{a.vps}:{staging}/release.json"])
    # Both checks must pass before an immutable directory is published.
    ssh(a.vps, '''set -euo pipefail
cd "$1"
printf '%s  %s\n' "$3" nfsu2.wasm "$4" nfsu2.json "$5" release.json | sha256sum -c - >&2
chmod 644 nfsu2.wasm nfsu2.json release.json
if [ -e "$2" ]; then
  cmp nfsu2.wasm "$2/nfsu2.wasm" && cmp nfsu2.json "$2/nfsu2.json" && cmp release.json "$2/release.json"
  rm nfsu2.wasm nfsu2.json release.json
  cd /
  rmdir "$1"
else
  mv -T "$1" "$2"
fi
''', staging, final, wasm_hash, json_hash, record_hash)
    wasm_path = f"/module/releases/{release_id}/nfsu2.wasm"
    print("Verified image and module; updating Coolify configuration.", flush=True)
    # PATCH calls change saved configuration, not the running container. Disable
    # auto-deploy in Coolify; only request deployment after both calls succeed.
    api(base, token, "PATCH", prefix + "/envs", {"key": "NFSU2_WASM", "value": wasm_path, "is_preview": False})
    api(base, token, "PATCH", prefix, {"docker_registry_image_name": repo,
                                       "docker_registry_image_tag": "sha-" + commit,
                                       "is_auto_deploy_enabled": False, "instant_deploy": False})
    result = api(base, token, "POST", "/deploy?" + urllib.parse.urlencode({"uuid": app}))
    deployments = result.get("deployments", [])
    if len(deployments) != 1 or not deployments[0].get("deployment_uuid"):
        raise ValueError(f"Coolify did not return one deployment UUID: {result}")
    deployment = deployments[0]["deployment_uuid"]
    if not re.fullmatch(r"[A-Za-z0-9-]+", deployment):
        raise ValueError("invalid deployment UUID from Coolify")
    for _ in range(120):
        status = api(base, token, "GET", "/deployments/" + deployment).get("status", "")
        if status == "finished":
            print(f"Coolify deployment {deployment} finished; verify the public /game.json and page.")
            return
        if status in ("failed", "cancelled", "cancelled-by-user"):
            raise ValueError(f"Coolify deployment {deployment}: {status}; inspect its logs")
        time.sleep(3)
    raise ValueError(f"Coolify deployment {deployment} did not finish in 6 minutes; inspect its status")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError, urllib.error.URLError) as exc:
        print(f"release: {exc}", file=sys.stderr)
        print("If Coolify configuration was already updated, check it before any later deployment.", file=sys.stderr)
        sys.exit(1)
