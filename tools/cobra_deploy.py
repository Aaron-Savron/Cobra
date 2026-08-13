#!/usr/bin/env python3
"""Build, validate, and publish Cobra artifacts.

The command is deliberately thin: Make owns packaging, GitHub CLI owns release
authentication, and GitHub Actions owns Pages deployment.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERSION = "1.0.0"
ARCHIVE = ROOT / f"cobra-v{VERSION}-linux-x86_64.tar.gz"


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    print("$", " ".join(args))
    return subprocess.run(args, cwd=ROOT, text=True, check=check)


def require(command: str) -> None:
    if not shutil.which(command):
        raise SystemExit(f"Missing required command: {command}")


def package() -> None:
    require("make")
    run("make", "dist")
    if not ARCHIVE.exists():
        raise SystemExit(f"Expected package was not created: {ARCHIVE.name}")
    print(f"Package ready: {ARCHIVE}")


def site_check() -> None:
    site = ROOT / ".cobra-site-check"
    if site.exists():
        shutil.rmtree(site)
    (site / "docs").mkdir(parents=True)
    shutil.copy(ROOT / "web/index.html", site / "index.html")
    shutil.copy(ROOT / "web/docs.html", site / "docs.html")
    shutil.copy(ROOT / "web/cobra_logo.jpg", site / "cobra_logo.jpg")
    for path in (ROOT / "docs").glob("*.md"):
        shutil.copy(path, site / "docs" / path.name)
    for name in ("CONTRIBUTING.md", "ROADMAP.md"):
        shutil.copy(ROOT / name, site / name)

    docs_html = (site / "docs.html").read_text()
    replacements = {
        "../docs/": "docs/",
        "../CONTRIBUTING.md": "CONTRIBUTING.md",
        "../ROADMAP.md": "ROADMAP.md",
    }
    for old, new in replacements.items():
        docs_html = docs_html.replace(old, new)
    docs_html = docs_html.replace('href="index.html"', 'href="./"')
    (site / "docs.html").write_text(docs_html)

    missing = []
    for document in (ROOT / "docs").glob("*.md"):
        if not (site / "docs" / document.name).exists():
            missing.append(document.name)
    if missing:
        raise SystemExit("Pages artifact is missing: " + ", ".join(missing))
    print(f"Pages check passed: {site}")


def publish(args: argparse.Namespace) -> None:
    require("git")
    package()
    if not args.push:
        print("Dry run complete. Re-run with --push to push the commit and tag.")
        return

    tag = f"v{VERSION}"
    run("git", "add", "Makefile", "install.sh", "README.md", "docs", "lib", "runtime")
    run("git", "commit", "-m", args.message, check=False)
    run("git", "tag", "-a", tag, "-m", f"Cobra {tag}")
    run("git", "push", "origin", "HEAD")
    run("git", "push", "origin", tag)
    print("GitHub Actions will publish Pages and CI artifacts from the push.")


def release(args: argparse.Namespace) -> None:
    require("gh")
    package()
    if not args.create:
        print("Release dry run complete. Re-run with --create to create the GitHub release.")
        return
    run("gh", "release", "create", f"v{VERSION}", str(ARCHIVE),
        "--title", f"Cobra v{VERSION}", "--generate-notes")


def main() -> int:
    parser = argparse.ArgumentParser(prog="cobra-deploy")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("package", help="build the Linux x86_64 release archive")
    sub.add_parser("site-check", help="validate the GitHub Pages artifact locally")
    publish_parser = sub.add_parser("publish", help="package and optionally push a version")
    publish_parser.add_argument("--push", action="store_true")
    publish_parser.add_argument("--message", default=f"Release Cobra v{VERSION}")
    release_parser = sub.add_parser("release", help="package and optionally create a GitHub release")
    release_parser.add_argument("--create", action="store_true")
    args = parser.parse_args()
    if args.command == "package":
        package()
    elif args.command == "site-check":
        site_check()
    elif args.command == "publish":
        publish(args)
    elif args.command == "release":
        release(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
