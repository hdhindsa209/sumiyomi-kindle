#!/usr/bin/env python3
"""Write an extension repository index.json from a sources/ directory.

    tools/ext/build_index.py sources/ --out repo/                 # copy the sources next to the index
    tools/ext/build_index.py sources/ --out . --in-place          # leave them where they are

Writes index.json with a SHA-256 for every file. Host the output anywhere that serves plain files (a GitHub raw
path works); the app is given the URL of index.json, and file paths in it are relative to that URL.
"""
import argparse, hashlib, json, pathlib, shutil, sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("sources", type=pathlib.Path, help="directory holding <id>/manifest.json + source.lua")
    ap.add_argument("--out", type=pathlib.Path, required=True, help="directory to write the repository into")
    ap.add_argument("--base", default="", help="optional absolute URL prefix for file paths")
    ap.add_argument("--in-place", action="store_true",
                    help="reference the sources where they are instead of copying them next to the index")
    args = ap.parse_args()

    entries = []
    for source_dir in sorted(p for p in args.sources.iterdir() if p.is_dir()):
        manifest_path, code_path = source_dir / "manifest.json", source_dir / "source.lua"
        if not manifest_path.exists() or not code_path.exists():
            print(f"skipping {source_dir.name}: not a source", file=sys.stderr)
            continue
        manifest = json.loads(manifest_path.read_text())
        if args.in_place:
            prefix = f"{source_dir.relative_to(args.out) if args.out in source_dir.parents else source_dir}/"
        else:
            out_dir = args.out / source_dir.name
            out_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(manifest_path, out_dir / "manifest.json")
            shutil.copy2(code_path, out_dir / "source.lua")
            prefix = f"{source_dir.name}/"
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        entries.append({
            "id": manifest["id"],
            "name": manifest["name"],
            "lang": manifest["lang"],
            "version": manifest["version"],
            "api_level": manifest.get("api_level", 1),
            "nsfw": bool(manifest.get("nsfw", False)),
            "manifest": f"{args.base}{prefix}manifest.json",
            "manifest_sha256": sha(manifest_path),
            "source": f"{args.base}{prefix}source.lua",
            "source_sha256": sha(code_path),
        })
        print(f"{manifest['id']} {manifest['version']}")

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "index.json").write_text(json.dumps({"format": 1, "sources": entries}, indent=2) + "\n")
    print(f"wrote {args.out / 'index.json'} ({len(entries)} sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
