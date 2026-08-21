"""Unit tests for bootstrap.py. Run from the repository root:

    python3 tools/test_bootstrap.py

Two things are worth testing here, and they are the two that can be wrong
without anybody noticing until a new machine is already set up badly:

  parsing      bootstrap reads the repository list and the on-disk layout out
               of CLAUDE.md rather than keeping its own copy. That is the right
               call, and it means a change to that document's shape breaks the
               bootstrap. These cases fail loudly when it does.

  branch rank  choosing which branch a fresh clone should move to. The first
               dry run of the real script ranked v0.1.3.1-old-knx as the newest
               branch of open_plc_arduino and would have checked it out.

Everything else in bootstrap.py either talks to the network, installs software,
or asks a person, so it is exercised by `--dry-run` against a real machine.

Exit 0 = all pass, 1 = at least one failed.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bootstrap as b  # noqa: E402

fails = 0


def check(name, got, expect):
    global fails
    ok = got == expect
    print("  [%s] %-56s got=%r" % ("PASS" if ok else "FAIL", name, got))
    if not ok:
        print("         expected=%r" % (expect,))
        fails += 1


class Args:
    dry_run = True
    yes = True
    no_install = False
    workspace = None
    skip = []


def test_parsing():
    print("=== CLAUDE.md is the single source ===")
    urls = b.parse_repo_urls()
    layout = b.parse_layout()

    check("the repository table still parses", len(urls) >= 5, True)
    check("every URL is SSH -- an HTTPS one clones and then fails on push",
          [u for u in urls.values()
           if not (u.startswith("git@") or u.startswith("ssh://"))], [])
    check("the layout block still parses", len(layout) >= 5, True)

    plan = b.planned_repos()
    check("a plan was produced", plan is not None, True)
    names = [n for n, _r, _u in (plan or [])]

    # The layout block is what decides. AI-Skills has a row in the table but is
    # deliberately not on disk: the plugin system fetches it. If it ever appears
    # here, the two halves of section 3 have stopped agreeing.
    check("AI-Skills is not cloned -- the plugin system fetches it",
          "AI-Skills" in names, False)
    check("package_index_json is not cloned either",
          "package_index_json" in names, False)
    check("the three required repos are all in the plan",
          sorted(n for n in names if n in
                 ("open_plc_cube_ide", "open_plc_arduino", "IAPTranfer_Tool")),
          ["IAPTranfer_Tool", "open_plc_arduino", "open_plc_cube_ide"])

    # The reference project lives one level down; a flat clone would break every
    # relative path that mentions it.
    rel = dict((n, r) for n, r, _u in (plan or []))
    check("Hello_World_OpenPLC goes under ref/",
          rel.get("Hello_World_OpenPLC"), "ref/Hello_World_OpenPLC")


def test_version_rank():
    print("=== which branch looks newest ===")
    check("master is not a version branch", b.version_rank("master"), None)
    check("main is not either", b.version_rank("main"), None)
    check("a feature branch is not either",
          b.version_rank("codex/fix-undefined-references"), None)

    # The bug this is here for.
    check("an -old- branch is rejected outright",
          b.version_rank("v0.1.3.1-old-knx"), None)
    check("so is .backup", b.version_rank("v0.1.2.1.backup"), None)
    check("so is .old", b.version_rank("v0.1.2.1.old"), None)
    check("so is -tmp", b.version_rank("v0.1.2-tmp"), None)

    def best(branches):
        ranked = sorted([(b.version_rank(x), x) for x in branches
                         if b.version_rank(x)], reverse=True)
        return ranked[0][1] if ranked else None

    check("the real branch list of open_plc_cube_ide picks v0.1.3.1",
          best(["master", "v0.1.2", "v0.1.2-lwip", "v0.1.2-old", "v0.1.2-tmp",
                "v0.1.2.1", "v0.1.3-dev", "v0.1.3.1"]), "v0.1.3.1")
    check("the real branch list of open_plc_arduino picks v0.1.3.1, not -old-knx",
          best(["codex/fix-x", "master", "v0.1.2.1", "v0.1.2.1.backup",
                "v0.1.2.1.old", "v0.1.3", "v0.1.3-dev", "v0.1.3-pre",
                "v0.1.3.1", "v0.1.3.1-old-knx"]), "v0.1.3.1")
    check("at the same version the plain name beats a qualified one",
          best(["v0.1.3-pre", "v0.1.3"]), "v0.1.3")
    check("more version components sort above fewer",
          best(["v0.1.3", "v0.1.3.1"]), "v0.1.3.1")
    check("nothing version-like gives no suggestion at all",
          best(["master", "gh-pages"]), None)


def main():
    b.ARGS = Args()
    test_parsing()
    print()
    test_version_rank()
    print()
    if fails:
        print("%d failure(s)" % fails)
        return 1
    print("all cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
