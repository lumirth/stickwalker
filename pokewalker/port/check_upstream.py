#!/usr/bin/env python3
"""Verify the fixed lumirth/pw source snapshot against its recorded hashes."""
import hashlib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
manifest = {}
for line in (root / 'upstream.sha256').read_text().splitlines():
    digest, name = line.split('  ', 1)
    manifest[name] = digest
actual = {str(path.relative_to(root / 'upstream'))
          for path in (root / 'upstream').rglob('*') if path.is_file()}
if actual != manifest.keys():
    raise SystemExit('Upstream file list changed; put native adaptations in port/')
for name, digest in manifest.items():
    if hashlib.sha256((root / 'upstream' / name).read_bytes()).hexdigest() != digest:
        raise SystemExit(f'Upstream changed: {name}; put native adaptations in port/')
