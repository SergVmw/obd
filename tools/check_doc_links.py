#!/usr/bin/env python3
"""Every documentation link must point at something that exists.

Two kinds of link are checked. Relative paths must resolve inside the
repository, and every GitHub blob/tree link must name a ref that really
exists: either a local tag, or a commit the current history can resolve.

The second check exists because removing an old release and pointing at its tag
was done by hand here and got one tag wrong: v0.4.3 never had a tag, so the
link 404'd while the three beside it worked. A 404 in a release note is
invisible to every other gate, because nothing in the build reads the
documentation.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "https://github.com/SergVmw/obd"

failures = []


def fail(message: str) -> None:
    failures.append(message)


documents = (
    sorted(ROOT.glob("*.md"))
    + sorted((ROOT / "docs").glob("*.md"))
    + sorted((ROOT / "releases").glob("*.md"))
)

tags = set(
    subprocess.run(
        ["git", "tag", "-l"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.split()
)

shallow = (
    subprocess.run(
        ["git", "rev-parse", "--is-shallow-repository"],
        cwd=ROOT, capture_output=True, text=True,
    ).stdout.strip()
    == "true"
)

# A repository link can only be judged against tags. Without any tag every ref
# looks broken, and reporting that as eleven broken links sends the reader
# after the documentation instead of after the checkout. A shallow clone that
# still has its tags is fine: tag refs resolve, and so does any recent commit.
if not tags:
    print(
        "cannot verify repository links: no tags in this checkout, so every ref "
        "would look broken",
        file=sys.stderr,
    )
    print(
        "check_doc_links.py needs tags; actions/checkout must use "
        "fetch-depth: 0",
        file=sys.stderr,
    )
    sys.exit(1)

SHALLOW_NOTE = (
    " (this checkout is shallow, so a commit ref may be absent from the clone "
    "rather than nonexistent)"
    if shallow
    else ""
)

# blob/<ref>/<path> and tree/<ref>/<path> against this repository. The ref is
# the segment between the kind and the path, and must survive the redirect to
# tree/<ref>/<path>.
external = re.compile(
    re.escape(REPOSITORY) + r"/(?:blob|tree)/([A-Za-z0-9._/-]+?)/([^)\s]*)/?"
)
link = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")

checked_local = 0
checked_external = 0

for document in documents:
    text = document.read_text(encoding="utf-8")
    for target in link.findall(text):
        if target.startswith(("http://", "mailto:")):
            continue
        if target.startswith(REPOSITORY):
            continue
        if target.startswith(("http", "#")):
            continue
        checked_local += 1
        resolved = (document.parent / target.split("#")[0].strip()).resolve()
        if not resolved.exists():
            fail(f"{document.name}: local link does not resolve: {target}")

    for ref, _path in external.findall(text):
        checked_external += 1
        if ref in tags:
            continue
        # Not a tag. It may still be a commit, which is the honest way to reach
        # a release that never got one.
        probe = subprocess.run(
            ["git", "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if probe.returncode != 0:
            fail(
                f"{document.name}: link names ref '{ref}', which is neither a tag "
                f"(known: {', '.join(sorted(tags))}) nor a commit in this history; "
                f"the URL will 404" + SHALLOW_NOTE
            )

if failures:
    print("documentation links do not resolve:", file=sys.stderr)
    for failure in failures:
        print(f"  - {failure}", file=sys.stderr)
    sys.exit(1)
print(
    f"Documentation links resolve: {checked_local} local, "
    f"{checked_external} repository refs, against {len(tags)} tags"
)
