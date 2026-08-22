"""Set up a new machine for this product, end to end, in one run.

    git clone git@github.com:haliboteda/open_plc_cube_ide.git
    python3 open_plc_cube_ide/tools/bootstrap.py        # python on Windows

Two commands, not one, and that is unavoidable: a script has to be on the
machine before it can run, and getting it there is the clone.

What it does, in order:

    1  check SSH reaches every host the clone URLs name
    2  clone the repositories that are missing
    3  check out the branch work is actually on -- NOT the default branch
    4  install the missing tools it is allowed to install, after showing you
       every command and asking once
    5  detect this machine's paths (hands over to IAPTranfer_Tool's
       init_machine.py, which asks you about whatever it cannot find)
    6  let a Claude Code session in one repo read the sibling repos
    6b sync AI-Skills/_shared/rules/ into ~/.claude/rules/, the only place
       that loads into every session of every project
    7  register the AI-Skills plugin marketplace and install /openplc:init
    8  verify, and say plainly what still needs a person

Flags:

    --dry-run     print every action, change nothing
    --yes         do not ask before installing; for unattended runs
    --no-install  skip step 4 entirely; report only
    --workspace D put the repositories under D instead of next to this repo

WHY THIS FILE LIVES HERE, and not in IAPTranfer_Tool/TestTool/tools/ with
every other script: it has to run before IAPTranfer_Tool exists. This repo is
the one you clone first, so this is the only place a bootstrap can be reached
from. Everything it can delegate, it delegates -- there is no second copy of
the settings table, the install commands, or the repository list here.

The repository list and the on-disk layout are PARSED OUT OF CLAUDE.md section
3, which is where they are documented for people. That table is the single
source; a copy here would be a second one, and this project has been bitten by
that often enough to have a rule about it. If the table's shape ever changes,
this script fails loudly instead of quietly using a stale list.

Two things it cannot do, by their nature:

  - git and Python 3 must already be there. You used git to clone this, and
    Python to run it, so in practice they are.
  - STM32CubeIDE is behind an ST account login and no package manager carries
    it. It gets reported, never faked.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

BOOT_REPO = Path(__file__).resolve().parent.parent
CLAUDE_MD = BOOT_REPO / "CLAUDE.md"

if sys.platform.startswith("win"):
    PLATFORM = "windows"
elif sys.platform == "darwin":
    PLATFORM = "macos"
else:
    PLATFORM = "linux"
IS_WIN = PLATFORM == "windows"

# A small copy of init_machine's output helpers. It cannot be imported yet --
# it lives in a repository this script may be about to clone.
def _c(text, code):
    if os.environ.get("NO_COLOR") or not sys.stdout.isatty():
        return text
    if IS_WIN:
        try:
            import ctypes
            k = ctypes.windll.kernel32
            k.SetConsoleMode(k.GetStdHandle(-11), 7)
        except Exception:
            return text
    return "\033[%sm%s\033[0m" % (code, text)


def Section(t):
    print()
    print(_c("===== " + t, "36"))


def Ok(t):
    print(_c(t, "32"))


def Warn(t):
    print(_c(t, "33"))


def Fail(t):
    print(_c(t, "31"))


STEPS = []          # (name, ok, detail) -- the closing summary
ARGS = None


def record(name, ok, detail):
    STEPS.append((name, ok, detail))


def run(cmd, cwd=None, capture=True, check=False, quiet=False, readonly=False):
    """Run a command. In --dry-run only the ones marked readonly actually run.

    Marking the queries readonly is what makes --dry-run worth having: a dry run
    that cannot even look at the branches has nothing to tell you about them.
    """
    shown = cmd if isinstance(cmd, str) else " ".join(str(c) for c in cmd)
    if ARGS.dry_run and not readonly:
        print("  would run: %s" % shown)
        return 0, "", ""
    if not quiet:
        print("  $ %s" % shown)
    try:
        r = subprocess.run(cmd, cwd=str(cwd) if cwd else None,
                           shell=isinstance(cmd, str),
                           capture_output=capture, text=True)
    except Exception as e:
        if check:
            raise
        return 1, "", str(e)
    return r.returncode, (r.stdout or ""), (r.stderr or "")


def git(repo, *args):
    """Read-only git queries only -- see run()'s readonly flag."""
    code, out, _err = run(["git", "-C", str(repo)] + list(args), quiet=True,
                          readonly=True)
    return out.strip() if code == 0 else ""


def ask(question, default=""):
    """Only when someone is there. Same two-part test as init_machine: an agent
    and a CI job both hold a real console, so isatty() alone is not enough, and
    a prompt nobody is watching hangs rather than failing."""
    if ARGS.yes:
        return default
    markers = [v for v in ("CLAUDECODE", "AI_AGENT", "CI", "GITHUB_ACTIONS",
                           "INIT_MACHINE_NO_INPUT") if os.environ.get(v)]
    if markers or not sys.stdin.isatty():
        why = ("automation markers: " + ", ".join(markers)) if markers \
              else "stdin is not a terminal"
        print("  (not asking -- %s; using %r)" % (why, default))
        return default
    suffix = " [%s]" % default if default else ""
    try:
        got = input("  %s%s: " % (question, suffix)).strip()
    except (EOFError, KeyboardInterrupt):
        print()
        return default
    return got or default


def yes_no(question, default=True):
    d = "Y/n" if default else "y/N"
    got = ask("%s (%s)" % (question, d), "").lower()
    if not got:
        return default
    return got.startswith("y")


# ------------------------------------------------ CLAUDE.md is the source
def parse_repo_urls():
    """{repo name: clone URL} from the table in CLAUDE.md section 3."""
    if not CLAUDE_MD.exists():
        Fail("  %s is missing -- is this really the open_plc_cube_ide clone?"
             % CLAUDE_MD)
        return {}
    urls = {}
    for line in CLAUDE_MD.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            continue
        name = re.match(r"\|\s*`([A-Za-z0-9_.-]+)`\s*\|", line)
        if not name:
            continue
        url = re.search(r"`((?:git@|ssh://)[^`]+\.git)`", line)
        if url:
            urls[name.group(1)] = url.group(1)
    return urls


def parse_layout():
    """[(repo name, path relative to the workspace)] from the fenced layout
    block in CLAUDE.md section 3. That block is the list of what belongs on
    disk: AI-Skills and package_index_json are deliberately not in it, because
    the plugin system fetches one and the other is only needed at release."""
    text = CLAUDE_MD.read_text(encoding="utf-8") if CLAUDE_MD.exists() else ""
    block = re.search(r"```\s*\n<workspace>/\n(.*?)```", text, re.S)
    if not block:
        return []
    out = []
    for raw in block.group(1).splitlines():
        rel = raw.strip().rstrip("/")
        if not rel or rel.startswith("#"):
            continue
        out.append((Path(rel).name, rel))
    return out


def planned_repos():
    urls = parse_repo_urls()
    layout = parse_layout()
    if not urls or not layout:
        Fail("  could not read the repository table or the layout block out of")
        Fail("  CLAUDE.md section 3. Its shape must have changed. Fix this script")
        Fail("  rather than pasting the list into it -- one source, by policy.")
        return None
    plan, unknown = [], []
    for name, rel in layout:
        if name in urls:
            plan.append((name, rel, urls[name]))
        else:
            unknown.append(name)
    if unknown:
        Fail("  the layout block lists %s, but the table above it gives no clone"
             % ", ".join(unknown))
        Fail("  URL for them. One of the two is out of date; fix CLAUDE.md.")
        return None
    return plan


# ------------------------------------------------------------ 1  ssh reach
def step_ssh(plan):
    Section("1  can SSH reach the hosts these URLs name")
    print("  Every URL here is SSH on purpose: GitHub no longer accepts password")
    print("  pushes, and an HTTPS remote clones fine and then fails on push.")
    hosts = []
    for _n, _r, url in plan:
        m = re.search(r"(?:git@|ssh://git@)([^:/]+)", url)
        if m and m.group(1) not in hosts:
            hosts.append(m.group(1))

    reachable = {}
    for host in hosts:
        # A working SSH handshake to a git host exits non-zero and says who you
        # are, so the exit code proves nothing -- the greeting does.
        code, out, err = run(["ssh", "-o", "BatchMode=yes",
                              "-o", "StrictHostKeyChecking=accept-new",
                              "-o", "ConnectTimeout=10", "-T", "git@" + host],
                             quiet=True, readonly=True)
        text = (out + err).lower()
        good = any(w in text for w in ("successfully authenticated", "you've successfully",
                                       "logged in", "welcome", "shell access"))
        reachable[host] = good
        if good:
            Ok("  %-28s reachable" % host)
        else:
            Fail("  %-28s NOT reachable" % host)
            for line in (out + err).strip().splitlines()[:3]:
                print("      %s" % line)

    if not all(reachable.values()):
        Warn("")
        Warn("  Fix SSH before going on. Do not switch to HTTPS.")
        Warn("    ssh-keygen -t ed25519 -C \"you@example.com\"      (once per machine)")
        Warn("    then add the PUBLIC key (~/.ssh/id_ed25519.pub) to:")
        Warn("      GitHub  -> Settings / SSH and GPG keys / New SSH key")
        Warn("      Forgejo -> Settings / SSH keys              (git.schaeffer-ag.de)")
        Warn("    check with: ssh -T git@<host>")
    record("SSH reaches every host", all(reachable.values()),
           ", ".join("%s=%s" % (h, "ok" if v else "FAILED")
                     for h, v in reachable.items()))
    return reachable


def find_existing(name, rel, workspace):
    """An existing clone, wherever it reasonably is.

    The layout block gives one place per repo, but a repo shared across projects
    is reasonably kept one level up from a single project's workspace -- which is
    exactly where AI-Skills sits on the machine this was written on. Cloning a
    second copy because it was not in the expected spot would be worse than
    useless: the two would drift, and the rules sync would follow the wrong one.
    """
    for cand in (workspace / rel, workspace / name,
                 workspace.parent / rel, workspace.parent / name):
        if (cand / ".git").is_dir():
            return cand
    return None


def step_clone(plan, workspace, reachable):
    Section("2  clone what is missing")
    got, failed = [], []
    for name, rel, url in plan:
        found = find_existing(name, rel, workspace)
        if found:
            where = "already here" if found == workspace / rel else "found at"
            Ok("  %-22s %-12s %s" % (name, where, found))
            got.append((name, found))
            continue
        target = workspace / rel
        host = re.search(r"(?:git@|ssh://git@)([^:/]+)", url)
        if host and not reachable.get(host.group(1), True):
            Warn("  %-22s skipped -- %s is not reachable" % (name, host.group(1)))
            failed.append(name)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        code, _o, err = run(["git", "clone", url, str(target)])
        if code == 0 or ARGS.dry_run:
            got.append((name, target))
        else:
            # Hardware lives only on the internal Forgejo, so a clone failure
            # off the company network is expected rather than broken.
            Fail("  %-22s clone failed" % name)
            for line in err.strip().splitlines()[-3:]:
                print("      %s" % line)
            failed.append(name)
    record("all repositories present", not failed,
           ("missing: " + ", ".join(failed)) if failed else
           "%d repositories" % len(got))
    return got


# ---------------------------------------------------------------- 3 branch
# Branch names that are somebody's dead end. Ranking these as "newest" is not a
# theoretical risk: the first dry run of this script picked
# v0.1.3.1-old-knx for open_plc_arduino and would have checked it out.
DEAD_END = re.compile(r"(?i)(^|[-_./])(old|backup|bak|tmp|temp|wip|deprecated|"
                      r"knx|lwip)([-_./]|$)")


def version_rank(branch):
    """Sort key for a version-looking branch name, or None if it is not one --
    so master and main can never win by accident, and neither can a dead end.

    At the same version the SHORTER name wins: v0.1.3.1 beats v0.1.3.1-something,
    because a suffix is a qualifier and the plain name is the mainline.
    """
    if DEAD_END.search(branch):
        return None
    m = re.match(r"^v?(\d+(?:\.\d+)*)", branch)
    if not m:
        return None
    return (tuple(int(p) for p in m.group(1).split(".")), -len(branch))


def step_branches(repos):
    Section("3  check out the branch work is on, not the default branch")
    print("  git clone gives you the remote's DEFAULT branch. On this product")
    print("  that is usually not where the work is, and nothing warns you: you")
    print("  just build a version-old tree. So each repo is confirmed here.")
    print()
    stayed_on_default = []
    for name, repo in repos:
        current = git(repo, "rev-parse", "--abbrev-ref", "HEAD") or "?"
        raw = git(repo, "branch", "-r")
        remote = []
        for line in raw.splitlines():
            b = line.strip()
            if "->" in b or not b.startswith("origin/"):
                continue
            remote.append(b[len("origin/"):])

        ranked = sorted([(version_rank(b), b) for b in remote
                         if version_rank(b)], reverse=True)
        highest = ranked[0][1] if ranked else None
        on_version = version_rank(current) is not None

        print("  %s" % name)
        print("    on now    : %s%s" % (current, "" if on_version else
                                        "   <- not a version branch"))
        print("    remote has: %s" % (", ".join(remote) or "(none)"))

        # The default is to KEEP what is checked out. The version heuristic is
        # a suggestion and nothing more: for open_plc_arduino the newest version
        # branch is not the one being worked on, so acting on the guess would
        # move you off the right branch rather than onto it.
        if on_version:
            if highest and highest != current:
                print("    fyi       : %s exists and is newer-looking" % highest)
            want = ask("    branch to use", current)
        elif highest:
            # A fresh clone lands here. This is the case worth stopping for.
            Warn("    You are on the branch the clone handed you. Work is on a")
            Warn("    version branch -- newest-looking is %s." % highest)
            want = ask("    branch to use (Enter = %s)" % highest, highest)
            if want == current:
                Warn("    staying on %s. Nothing will warn you again." % current)
        else:
            print("    no version branches here, so this one is it")
            want = current

        if want and want != current:
            code, _o, err = run(["git", "-C", str(repo), "checkout", want])
            if code == 0:
                Ok("    -> %s" % want)
                current = want
            else:
                Fail("    could not check out %s: %s" % (want, err.strip()[:120]))
        stayed_on_default.append((name, current, version_rank(current) is None
                                  and bool(highest)))
        print()

    risky = [n for n, _c, bad in stayed_on_default if bad]
    record("branches confirmed", not risky,
           ("still on a non-version branch: " + ", ".join(risky)) if risky
           else ", ".join("%s=%s" % (n, c) for n, c, _b in stayed_on_default))


# --------------------------------------------------------------- 4 install
def load_init_machine(repos):
    """Import init_machine from the cloned tool repo. Everything about tools --
    what to check, how to install it, where things live -- is defined there."""
    tool = dict(repos).get("IAPTranfer_Tool")
    if not tool:
        return None
    tools_dir = Path(tool) / "TestTool" / "tools"
    if not (tools_dir / "init_machine.py").exists():
        return None
    sys.path.insert(0, str(tools_dir))
    try:
        import init_machine
        return init_machine
    except Exception as e:
        Fail("  could not import init_machine.py: %s" % e)
        return None


def step_install(im):
    Section("4  install the missing tools")
    if ARGS.no_install:
        Warn("  --no-install: reporting only")
    missing = []
    for name, what_for, check, required, install in im.PREREQS:
        try:
            ok, detail = check()
        except Exception as e:
            ok, detail = False, "check raised %s" % e
        if ok:
            Ok("  %-14s %s" % (name, detail))
            continue
        missing.append((name, what_for, install[im.PLATFORM], required))
        Warn("  %-14s missing -- %s" % (name, detail))

    if not missing:
        record("tools installed", True, "nothing was missing")
        return

    runnable = [m for m in missing if m[2]["auto"]]
    by_hand = [m for m in missing if not m[2]["auto"]]

    if by_hand:
        Section("   these need a person")
        for name, what_for, entry, _req in by_hand:
            print()
            Warn("   %s" % name)
            print("     needed for : %s" % what_for)
            print("     do this    : %s" % entry["cmd"])

    if not runnable or ARGS.no_install:
        record("tools installed", False,
               "%d still missing, %d of them installable by script"
               % (len(missing), len(runnable)))
        return

    Section("   these can be installed now")
    for name, _w, entry, _r in runnable:
        print("   %-14s %s" % (name, entry["cmd"]))
    print()
    if PLATFORM != "windows" and any("sudo" in m[2]["cmd"] for m in runnable):
        Warn("   Some of these are sudo. You will be asked for your password by")
        Warn("   the command itself, not by this script.")
    if not yes_no("   run these now?", True):
        Warn("   skipped at your request")
        record("tools installed", False, "declined at the prompt")
        return

    done, failed = [], []
    for name, _w, entry, _r in runnable:
        print()
        print("  --- %s" % name)
        code, out, err = run(entry["cmd"], capture=False)
        if code == 0 or ARGS.dry_run:
            Ok("  %s: done" % name)
            done.append(name)
        else:
            Fail("  %s: the install command exited %d" % (name, code))
            failed.append(name)

    if failed:
        Warn("")
        Warn("  A failed install here is usually one of three things: no package")
        Warn("  manager on PATH (winget/brew/snap), no network, or the package")
        Warn("  name moved. Install those by hand and re-run -- this script is")
        Warn("  safe to run again.")

    # Re-check, because "the installer exited 0" and "this process can see it"
    # are different claims. A new PATH entry does not reach a process that is
    # already running, so a fresh winget or apt install is routinely invisible
    # until the next terminal -- and reporting success here would send you off
    # to debug the wrong thing.
    Section("   what is visible now")
    invisible = []
    for name, _w, check, _r, _i in [p for p in im.PREREQS if p[0] in done]:
        try:
            ok, detail = check()
        except Exception as e:
            ok, detail = False, "check raised %s" % e
        if ok:
            Ok("  %-14s %s" % (name, detail))
        else:
            invisible.append(name)
            Warn("  %-14s installed, but not visible to this process" % name)
    if invisible:
        Warn("")
        Warn("  %s went in but cannot be seen from here." % ", ".join(invisible))
        Warn("  A new PATH entry does not reach a process that is already running.")
        Warn("  Open a NEW terminal and run this script again -- it keeps what is")
        Warn("  already right, so the second run is short.")

    record("tools installed", not failed and not invisible,
           "installed %s%s%s" % (", ".join(done) or "nothing",
                                 ("; FAILED: " + ", ".join(failed)) if failed else "",
                                 ("; needs a new terminal: " + ", ".join(invisible))
                                 if invisible else ""))


# ------------------------------------------------------ 5+6 paths and dirs
def step_paths(repos):
    Section("5  detect this machine's paths")
    tool = dict(repos).get("IAPTranfer_Tool")
    if not tool:
        record("machine config written", False, "IAPTranfer_Tool is not here")
        return False
    testtool = Path(tool) / "TestTool"
    runner = sys.executable
    print("  init_machine.py searches first and asks only about what it cannot")
    print("  find, showing where it already looked. Answer, or press Enter to")
    print("  skip one -- skipping only limits what can run.")
    code, _o, _e = run([runner, "tools/init_machine.py"], cwd=testtool, capture=False)
    ok = code == 0 or ARGS.dry_run
    record("machine config written", ok,
           "config/machine.py and machine.ps1" if ok else
           "init_machine exited %d -- something required is still missing" % code)

    Section("6  let a session in one repo read the others")
    code, _o, _e = run([runner, "tools/init_machine.py", "--write-claude-dirs"],
                       cwd=testtool, capture=False)
    record("sibling repos granted to Claude Code", code == 0 or ARGS.dry_run,
           "permissions.additionalDirectories in each repo's settings.local.json")
    return ok


# ------------------------------------------------------------ 6b user rules
# Standing rules -- "always mark hands-on steps", "always cite the path" -- are
# not tasks, so they cannot be skills: a skill loads when it is invoked or looks
# relevant. A plugin has no always-on slot either. The only mechanism that loads
# something into every session of every project is ~/.claude/rules/, and that is
# machine-local.
#
# So the text lives in git, in AI-Skills/_shared/rules/, and gets copied here.
# The copy carries a header saying where it came from and how to refresh it: a
# GENERATED copy of a tracked file is not the drift this project guards against
# -- two hand-maintained copies are.
RULES_HEADER = (
    "<!-- GENERATED COPY -- do not edit here.\n"
    "     Source : %s\n"
    "     Refresh: python3 open_plc_cube_ide/tools/bootstrap.py --skip 1 --skip 2 "
    "--skip 3 --skip 4 --skip 5 --skip 7 --skip 8\n"
    "     Why a copy: ~/.claude/rules/ is the only place that loads into every\n"
    "     session of every project, and it is machine-local. -->\n\n")


def step_user_rules(repos):
    Section("6b  working agreements that apply to every project")
    skills_repo = dict(repos).get("AI-Skills")
    if not skills_repo:
        Warn("  AI-Skills is not on disk, so there is nothing to sync.")
        record("user-level rules synced", False, "AI-Skills not cloned")
        return
    src = Path(skills_repo) / "_shared" / "rules"
    if not src.is_dir():
        Warn("  %s does not exist" % src)
        record("user-level rules synced", False, "no _shared/rules in AI-Skills")
        return

    dest = Path.home() / ".claude" / "rules"
    print("  %s" % src)
    print("    -> %s   (loads in every session, every project)" % dest)

    files = sorted(src.glob("*.md"))
    if not files:
        record("user-level rules synced", False, "no rule files found")
        return

    if not ARGS.dry_run:
        dest.mkdir(parents=True, exist_ok=True)
    written, unchanged = [], []
    for f in files:
        body = RULES_HEADER % f.as_posix() + f.read_text(encoding="utf-8")
        target = dest / f.name
        if target.exists() and target.read_text(encoding="utf-8") == body:
            unchanged.append(f.name)
            print("    %-28s unchanged" % f.name)
            continue
        if ARGS.dry_run:
            print("    %-28s would write" % f.name)
            written.append(f.name)
            continue
        # A file here that somebody edited by hand is still overwritten -- it is
        # a generated copy and says so -- but not silently.
        if target.exists():
            Warn("    %-28s replacing the existing copy" % f.name)
        target.write_text(body, encoding="utf-8", newline="\n")
        Ok("    %-28s written" % f.name)
        written.append(f.name)

    record("user-level rules synced", True,
           "%d written, %d already current" % (len(written), len(unchanged)))


# ----------------------------------------------------------------- 7 skill
def find_claude():
    hit = shutil.which("claude")
    if hit:
        return hit
    home = Path.home()
    guesses = [home / ".local" / "bin" / ("claude.exe" if IS_WIN else "claude")]
    ext = home / ".vscode" / "extensions"
    if ext.is_dir():
        for d in sorted(ext.glob("anthropic.claude-code-*")):
            guesses.append(d / "resources" / "native-binary" /
                           ("claude.exe" if IS_WIN else "claude"))
    for g in guesses:
        if g.exists():
            return str(g)
    return None


def step_skill():
    Section("7  the /openplc:init skill")
    marketplace = "haliboteda/AI-Skills"
    plugin = "openplc@ai-skills"
    claude = find_claude()
    if not claude:
        Warn("  the claude CLI is not on PATH, so this step is yours:")
        print("    claude plugin marketplace add %s" % marketplace)
        print("    claude plugin install %s" % plugin)
        record("skill installed", False, "claude CLI not found")
        return

    print("  using %s" % claude)
    # Adding the marketplace explicitly, rather than relying on the declaration
    # in each repo's committed settings.json: that one only takes effect after
    # you have trusted the folder interactively, and this script cannot do that.
    code, _o, err = run([claude, "plugin", "marketplace", "add", marketplace])
    if code != 0 and "already" not in (err or "").lower() and not ARGS.dry_run:
        Warn("  could not add the marketplace: %s" % (err or "").strip()[:200])
        Warn("  SSH is the usual cause -- a github.com owner/repo shorthand clones")
        Warn("  over SSH, and Claude Code suppresses interactive SSH prompts, so")
        Warn("  github.com must already be in known_hosts.")
    code, _o, err = run([claude, "plugin", "install", plugin])
    ok = code == 0 or ARGS.dry_run
    if not ok:
        Warn("  install failed: %s" % (err or "").strip()[:200])
    record("skill installed", ok, plugin)


# -------------------------------------------------------------- 8 verify
def step_verify(repos):
    Section("8  verify")
    tool = dict(repos).get("IAPTranfer_Tool")
    if not tool:
        record("verified", False, "IAPTranfer_Tool is not here")
        return
    testtool = Path(tool) / "TestTool"
    code, _o, _e = run([sys.executable, "tools/common.py", "--probe"],
                       cwd=testtool, capture=False)
    record("A0 probe", code == 0 or ARGS.dry_run, "what this machine resolves to")

    pwsh = shutil.which("pwsh") or (shutil.which("powershell") if IS_WIN else None)
    if not pwsh:
        Warn("  no PowerShell, so not one test case can run yet. Every test script")
        Warn("  here is still PowerShell; the rewrite to Python is M7, tracked in")
        Warn("  docs/work/M7-python-scripts.md.")
        record("selfcheck", False, "no PowerShell on this machine")
        return
    code, _o, _e = run([pwsh, "-NoProfile", "-ExecutionPolicy", "Bypass",
                        "-File", "./tools/selfcheck.ps1"], cwd=testtool, capture=False)
    record("selfcheck", code == 0 or ARGS.dry_run,
           "all host-side checks" if code == 0 else
           "selfcheck reported failures -- read its summary above")


def summary(repos):
    Section("summary")
    width = max(len(n) for n, _o, _d in STEPS) if STEPS else 10
    for name, ok, detail in STEPS:
        line = "  %-*s  %s" % (width, name, detail)
        (Ok if ok else Warn)(line)

    bad = [n for n, ok, _d in STEPS if not ok]
    print()
    if not bad:
        Ok("  This machine is ready. Open a session and start with CLAUDE.md.")
    else:
        Warn("  Still open: %s" % ", ".join(bad))
        print()
        print("  This script is safe to run again -- it keeps what is already")
        print("  right and only fills in the rest.")

    print()
    print("  Two things no script can do for you:")
    print("    - Open Claude Code interactively once in a repo and accept the")
    print("      trust dialog. Until you do, every committed setting in")
    print("      .claude/settings.json is ignored, including the marketplace.")
    print("    - Install STM32CubeIDE: the download needs a free ST account and")
    print("      no package manager carries it.")
    if PLATFORM != "windows":
        print("    - Serial ports need group membership: sudo usermod -aG dialout")
        print("      $USER, then log out and back in. A new shell is not enough.")
    print()
    print("  Then: /openplc:init in a session, any time you want this re-checked.")


def main():
    global ARGS
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true",
                    help="print every action, change nothing")
    ap.add_argument("--yes", action="store_true",
                    help="do not ask; take the default for every question")
    ap.add_argument("--no-install", action="store_true",
                    help="never install anything; report what is missing")
    ap.add_argument("--workspace", metavar="DIR",
                    help="where the repositories go (default: next to this repo)")
    ap.add_argument("--skip", action="append", default=[], metavar="N",
                    help="skip step N; repeatable")
    ARGS = ap.parse_args()

    workspace = Path(ARGS.workspace).expanduser().resolve() if ARGS.workspace \
        else BOOT_REPO.parent
    skip = set(ARGS.skip)

    Section("bootstrap")
    print("  platform    %s  (Python %d.%d.%d)" % ((PLATFORM,) + sys.version_info[:3]))
    print("  this repo   %s" % BOOT_REPO)
    print("  workspace   %s" % workspace)
    if ARGS.dry_run:
        Warn("  --dry-run: nothing will be changed")

    plan = planned_repos()
    if not plan:
        return 2
    print("  repositories to have, from CLAUDE.md section 3:")
    for name, rel, url in plan:
        print("    %-22s %-30s %s" % (name, rel, url))

    reachable = step_ssh(plan) if "1" not in skip else {}
    repos = step_clone(plan, workspace, reachable) if "2" not in skip else \
        [(n, p) for n, r, _u in plan
         for p in [find_existing(n, r, workspace)] if p]

    if "3" not in skip:
        step_branches(repos)

    im = load_init_machine(repos)
    if im and "4" not in skip:
        step_install(im)
    elif not im:
        Warn("\n  init_machine.py is not available, so steps 4 to 6 and 8 cannot run.")
        record("tools installed", False, "IAPTranfer_Tool missing or unreadable")

    if im and "5" not in skip:
        step_paths(repos)
    if "6b" not in skip and "6" not in skip:
        step_user_rules(repos)
    if "7" not in skip:
        step_skill()
    if im and "8" not in skip:
        step_verify(repos)

    summary(repos)
    return 0 if all(ok for _n, ok, _d in STEPS) else 1


if __name__ == "__main__":
    sys.exit(main())
