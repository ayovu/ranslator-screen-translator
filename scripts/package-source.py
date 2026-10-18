#!/usr/bin/env python3
"""Create a source release from an explicit allowlist, without local profiles or builds."""
import hashlib
from pathlib import Path
import tarfile
import tomllib

root = Path(__file__).resolve().parent.parent
manifest = tomllib.loads((root / "Cargo.toml").read_text())
name = "ranslator-" + manifest["package"]["version"]
output = root / "dist/source" / (name + ".tar.gz")
output.parent.mkdir(parents=True, exist_ok=True)
entries = (
    "AGENTS.md", "README.md", "CONTRIBUTING.md", "SECURITY.md", "LICENSE", ".gitignore",
    ".clang-format", ".containerignore", ".dockerignore", "Cargo.toml", "Cargo.lock", "build.rs", "rustfmt.toml",
    "config.example.json", "src", "native", "tests", "assets", "translations", "scripts", "packaging",
    "docs", "integrations", ".github/ISSUE_TEMPLATE", ".github/pull_request_template.md",
)


def include(info):
    parts = Path(info.name).parts
    if "__pycache__" in parts or info.name.endswith((".pyc", ".qm", ".user", ".log")):
        return None
    if info.issym() or info.islnk():
        raise RuntimeError("Review source release symlink before packaging: " + info.name)
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    return info


with tarfile.open(output, "w:gz") as archive:
    for entry in entries:
        path = root / entry
        if path.exists():
            archive.add(path, arcname=name + "/" + entry, filter=include)
digest = hashlib.sha256(output.read_bytes()).hexdigest()
output.with_suffix(output.suffix + ".sha256").write_text(digest + "  " + output.name + "\n")
print(output)
