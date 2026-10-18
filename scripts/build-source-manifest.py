#!/usr/bin/env python3
"""Record the exact build/test/packaging inputs used by a portable release."""
import hashlib
from pathlib import Path
import sys

root = Path(__file__).resolve().parent.parent
output = Path(sys.argv[1])
paths = []
for entry in ('Cargo.toml', 'Cargo.lock', 'build.rs', 'src', 'native', 'tests', 'assets', 'translations',
              'packaging', 'scripts', '.containerignore', '.dockerignore'):
    path = root / entry
    paths.extend(path.rglob('*') if path.is_dir() else [path])
records = []
for path in sorted(paths):
    if not path.is_file() or '__pycache__' in path.parts or path.suffix in {'.pyc', '.qm'}:
        continue
    if path.is_symlink():
        raise SystemExit('Review source symlink before packaging: ' + str(path))
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    records.append(digest + '  ' + path.relative_to(root).as_posix())
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text('\n'.join(records) + '\n')
print(f'Recorded {len(records)} build input hashes in {output}')
