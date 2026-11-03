#!/usr/bin/env python3
"""Apply a version-checked local colour-selection fix to a staged Control web app.

Original bundles are retained for rollback. New content gets new URLs, including
the bootstrap, so immutable HTTP caches cannot hide the fix. Unknown builds are
rejected before any write. This does not alter the signed Control executable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


SUPPORTED = {
    "a6f69b16f1c3d69e25b3fb1d86a2e326e9c4945d9365f9b19dd5ec2133780353": ("aFq", "aIm"),
    "b347974c91352c2f912471e71659a4d2f96d69c9772a9fc2430f4dee7ad577d5": ("aFq", "aIm"),
    "7dcdedf0e44126953bd5398e78561224a977d767f62e3463d4bbb777debc9c1c": ("aGL", "aJo"),
    "e52097598c44335f19f5060f5807f9a5a76ebb08159a2b9d177bcb2d6f9e9a21": ("aGL", "aJo"),
}
MANIFEST = "orca-colour-match-patch.json"
PINNED_RELEASE_SHA256 = "96109093573873627edd92bc538d46eaa0f46470f337a8d1f281e0f4db07f657"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def patch_bundle(data: bytes) -> bytes:
    policy = Path(__file__).with_name("filament_colour_match.js").read_text(encoding="utf-8")
    if digest(data) == PINNED_RELEASE_SHA256:
        text = data.decode("utf-8")
        replacements = {
            "else g=this.aJR(h)": "else g=this.aJR(h,k)",
            'aJR(a){var s,r,q,p=A.Z(a)': 'aJR(a,source){var s,r,q,p=A.Z(a)',
            "B.b.hp(o,new A.aTB())\nreturn o.length===0?null:B.b.ga1(o)},": (
                "B.b.hp(o,new A.aTB())\n" + policy + "\n"
                "function singleColour(values){return values.length===1?'#'+('000000'+(values[0]&16777215).toString(16)).slice(-6):null;}\n"
                "var options=[];\n"
                "for(var index=0;index<o.length;++index){var slot=o[index].a;"
                "options.push({colour:singleColour(slot.f.length?slot.f:A.xf(slot.e)),id:slot.a,distance:o[index].e});}\n"
                "var selected=selectFilamentColour(singleColour(source.gaqO()),options);\n"
                "return selected<0?null:o[selected]},"
            ),
        }
        for original, replacement in replacements.items():
            if text.count(original) != 1:
                raise RuntimeError("Unexpected pinned release selector layout")
            text = text.replace(original, replacement)
        return text.encode("utf-8")
    symbols = SUPPORTED.get(digest(data))
    if symbols is None:
        raise RuntimeError("Unsupported Control web bundle; review the new matcher before applying this patch")
    selector, distance = symbols
    text = data.decode("utf-8")
    start = text.index(f"{selector}(a,b){{var s,r,q,p,o,n,m,l")
    end = text.index("return q},", start) + len("return q},")
    original = text[start:end]
    if len(original) > 500 or original.count(f"this.{distance}(r,n.f)") != 1:
        raise RuntimeError("Unexpected colour selector layout")
    replacement = (
        f"{selector}(a,b){{\n{policy}\n"
        "var options=[];\n"
        f"for(var i=0;i<b.length;++i) options.push({{colour:b[i].f,id:b[i].e,distance:this.{distance}(a.x,b[i].f)}});\n"
        "var selected=selectFilamentColour(a.x,options);\n"
        "return selected<0?null:b[selected]},"
    )
    return (text[:start] + replacement + text[end:]).encode("utf-8")


def patch_web(root: Path) -> dict:
    root = root.resolve()
    manifest_path = root / MANIFEST
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for name, expected in manifest["patched_hashes"].items():
            if Path(name).name != name or digest((root / name).read_bytes()) != expected:
                raise RuntimeError("Previously patched files changed; restore the original bundle before reapplying")
        return manifest
    index = (root / "index.html").read_bytes()
    bootstrap_names = re.findall(rb'src="(flutter_bootstrap(?:\.[a-f0-9]+)?\.js)"', index)
    if len(bootstrap_names) != 1:
        raise RuntimeError("Expected exactly one content-hashed Flutter bootstrap")
    bootstrap_name = bootstrap_names[0].decode()
    bootstrap = (root / bootstrap_name).read_bytes()
    main_names = re.findall(rb'"mainJsPath":"(main\.(?:[a-f0-9]+|dart)\.js)"', bootstrap)
    if len(main_names) != 1:
        raise RuntimeError("Expected exactly one JavaScript entrypoint")
    main_name = main_names[0].decode()
    main = (root / main_name).read_bytes()
    patched_main = patch_bundle(main)
    new_main_name = f"main.{digest(patched_main)[:16]}.js"
    patched_bootstrap = bootstrap.replace(main_name.encode(), new_main_name.encode())
    new_bootstrap_name = f"flutter_bootstrap.{digest(patched_bootstrap)[:16]}.js"
    patched_index = index.replace(main_name.encode(), new_main_name.encode()).replace(
        bootstrap_name.encode(), new_bootstrap_name.encode()
    )
    backup = root / "index.before-colour-match.html"
    if backup.exists():
        raise RuntimeError("An index backup already exists without a patch manifest; inspect it before retrying")
    outputs = {new_main_name: patched_main, new_bootstrap_name: patched_bootstrap, "index.html": patched_index}
    manifest = {
        "version": 1,
        "original_main": main_name,
        "original_sha256": digest(main),
        "index_backup": backup.name,
        "patched_hashes": {name: digest(content) for name, content in outputs.items()},
    }
    backup.write_bytes(index)
    for name, content in outputs.items():
        (root / name).write_bytes(content)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def restore_web(root: Path) -> None:
    root = root.resolve()
    manifest_path = root / MANIFEST
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    # Verify first, so rollback cannot overwrite unrelated subsequent changes.
    patch_web(root)
    backup_name = manifest["index_backup"]
    if Path(backup_name).name != backup_name:
        raise RuntimeError("Invalid index backup path")
    (root / "index.html").write_bytes((root / backup_name).read_bytes())
    # Leave generated scripts in place: an open page may still be loading them.
    (root / backup_name).unlink()
    manifest_path.unlink()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("web_dir", type=Path)
    parser.add_argument("--restore", action="store_true", help="restore the original entrypoint after checking the patch")
    args = parser.parse_args()
    if args.restore:
        restore_web(args.web_dir)
        print("Restored original web entrypoint")
    else:
        print(json.dumps(patch_web(args.web_dir), indent=2))
