#!/usr/bin/env python3
"""Bump FetchContent GIT_TAG lines to latest GitHub release tag, else newest v* tag."""
from __future__ import annotations

import json
import os
import re
import sys
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CMAKE = os.path.join(ROOT, "CMakeLists.txt")

# (FetchContent_Declare name, owner/repo)
DEPS = [
    ("CLI11", "CLIUtils/CLI11"),
    ("json", "nlohmann/json"),
    ("httplib", "yhirose/cpp-httplib"),
    ("Catch2", "catchorg/Catch2"),
]


def api_json(path: str) -> object | None:
    req = urllib.request.Request(
        f"https://api.github.com{path}",
        headers={
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "mnemon-cpp-bump-fetchcontent",
            **(
                {"Authorization": f"Bearer {t}"}
                if (t := os.environ.get("GITHUB_TOKEN"))
                else {}
            ),
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.load(resp)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def latest_tag(repo: str) -> str | None:
    data = api_json(f"/repos/{repo}/releases/latest")
    if isinstance(data, dict) and data.get("tag_name"):
        return str(data["tag_name"])
    data = api_json(f"/repos/{repo}/tags?per_page=30")
    if not isinstance(data, list):
        return None
    for item in data:
        name = item.get("name", "")
        if re.match(r"^v\d", name):
            return str(name)
    return None


def main() -> int:
    with open(CMAKE, encoding="utf-8") as f:
        text = f.read()
    new_text = text
    for decl, repo in DEPS:
        pat = (
            rf"(FetchContent_Declare\({re.escape(decl)}\n"
            r"  GIT_REPOSITORY https://github\.com/[^\n]+\n"
            r"  GIT_TAG )([^\n]+)"
        )
        m = re.search(pat, new_text)
        if not m:
            print(f"error: no FetchContent block for {decl}", file=sys.stderr)
            return 1
        cur = m.group(2).strip()
        tag = latest_tag(repo)
        if not tag:
            print(f"error: could not resolve tag for {repo}", file=sys.stderr)
            return 1
        if tag != cur:
            new_text = new_text[: m.start(2)] + tag + new_text[m.end(2) :]
            print(f"{decl} ({repo}): {cur} -> {tag}")
    if new_text != text:
        with open(CMAKE, "w", encoding="utf-8") as f:
            f.write(new_text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
