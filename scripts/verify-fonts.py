#!/usr/bin/env python3
"""Verify bundled font source hashes and corresponding license notices, without network access."""
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent.parent
directory = root / 'assets/fonts/noto'
sources = json.loads((directory / 'sources.json').read_text())
for item in sources:
    path = directory / item['file']
    assert hashlib.sha256(path.read_bytes()).hexdigest() == item['sha256'], f'Font/notice changed: {path}'
    assert 'raw.githubusercontent.com/' in item['url'] and '/main/' not in item['url'], item
resources = {node.attrib.get('alias') for node in ET.parse(root / 'native/resources.qrc').iter('file')}
for item in sources:
    if item['file'].endswith(('.ttf', '.ttc')):
        assert 'fonts/' + item['file'] in resources, f'Font missing from Qt resources: {item["file"]}'
    else:
        assert 'OPEN FONT LICENSE' in (directory / item['file']).read_text(), item['file']
print('PASS: pinned Noto font archives, OFL notices, and embedded Qt resources.')
