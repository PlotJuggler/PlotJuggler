#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Every `path#token` citation in resources/feature_catalog.json names a file that contains the token.

Same rule as FeatureCatalogResourceTest.EvidenceTokensAppearInTheCitedFiles, run at commit time: the catalog cites
docs (CLAUDE.md, docs/) as well as code, and docs-only commits reach main without a build.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
doc = json.loads((ROOT / "resources/feature_catalog.json").read_text(encoding="utf-8"))
errors = []
for entry in doc.get("core", []) + doc.get("limitations", []):
    for citation in entry.get("evidence", "").split("; "):
        path, _, token = citation.partition("#")
        if not path or not token:
            errors.append(f"{entry.get('id', '?')}: malformed citation {citation!r}")
            continue
        try:
            text = (ROOT / path).read_text(encoding="utf-8")
        except OSError:
            errors.append(f"{entry.get('id', '?')}: {path} cannot be read")
            continue
        if token not in text:
            errors.append(f"{entry.get('id', '?')}: {path} no longer contains {token!r}")
for e in errors:
    print(f"resources/feature_catalog.json: {e}", file=sys.stderr)
if errors:
    print("Update the citation, or restore the cited text if its removal was unintended.", file=sys.stderr)
sys.exit(1 if errors else 0)
