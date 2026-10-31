#!/usr/bin/env python3
"""gen_version.py - numéro de version automatique pour les projets decomp/recomp.

Usage (depuis la racine du projet, là où se trouvent VERSION et project.ini) :
    python gen_version.py                 -> génère version.h + version.rc
    python gen_version.py bump patch      -> 0.9.0 -> 0.9.1   (ou minor / major)
    python gen_version.py show            -> affiche la version complète
    python gen_version.py --dir X --out Y -> projet en X, fichiers générés dans Y

Source de vérité : fichier VERSION (MAJ.MIN.PAT) + git (n° de build = nb de commits,
hash court, '.dirty' si modifications). Pas de date dans les fichiers générés, donc
version.h ne change que si la version change (pas de recompilation inutile).
Sans git : compteur dans .build_number (incrémenté avec 'bump' uniquement).
"""
import argparse, configparser, os, re, subprocess, sys

def git(args, cwd):
    try:
        return subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True, timeout=10).stdout.strip()
    except Exception:
        return ""

def read_version(d):
    p = os.path.join(d, "VERSION")
    if not os.path.exists(p):
        open(p, "w").write("0.1.0\n")
    m = re.match(r"\s*(\d+)\.(\d+)\.(\d+)", open(p).read())
    if not m:
        sys.exit("VERSION doit contenir MAJ.MIN.PAT (ex. 0.9.0)")
    return [int(x) for x in m.groups()]

def build_info(d):
    if git(["rev-parse", "--is-inside-work-tree"], d) == "true":
        n = int(git(["rev-list", "--count", "HEAD"], d) or 0)
        h = git(["rev-parse", "--short=7", "HEAD"], d)
        dirty = bool(git(["status", "--porcelain", "--untracked-files=no"], d))
        return n, h, dirty
    p = os.path.join(d, ".build_number")
    n = int(open(p).read().strip() or 0) if os.path.exists(p) else 0
    return n, "", False

def write_if_changed(path, text):
    if os.path.exists(path) and open(path, encoding="utf-8").read() == text:
        return False
    open(path, "w", encoding="utf-8", newline="\n").write(text)
    return True

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", nargs="?", default="gen", choices=["gen", "bump", "show"])
    ap.add_argument("part", nargs="?", default="patch", choices=["major", "minor", "patch"])
    ap.add_argument("--dir", default=".")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    d, out = os.path.abspath(a.dir), os.path.abspath(a.out or a.dir)

    cp = configparser.ConfigParser()
    cp.read(os.path.join(d, "project.ini"), encoding="utf-8")
    pj = cp["project"] if cp.has_section("project") else {}
    prefix = pj.get("prefix", os.path.basename(d).upper().replace(" ", "_")[:8] or "PRJ")
    name = pj.get("name", os.path.basename(d))
    exe = pj.get("exe", "game.exe")
    desc = pj.get("description", name)

    maj, mnr, pat = read_version(d)
    if a.cmd == "bump":
        if a.part == "major": maj, mnr, pat = maj + 1, 0, 0
        elif a.part == "minor": mnr, pat = mnr + 1, 0
        else: pat += 1
        open(os.path.join(d, "VERSION"), "w").write(f"{maj}.{mnr}.{pat}\n")
        if not os.path.isdir(os.path.join(d, ".git")) and git(["rev-parse", "--is-inside-work-tree"], d) != "true":
            p = os.path.join(d, ".build_number")
            n = int(open(p).read().strip() or 0) if os.path.exists(p) else 0
            open(p, "w").write(str(n + 1))
    build, h, dirty = build_info(d)
    short = f"{maj}.{mnr}.{pat}"
    full = f"{short}+b{build}" + (f".g{h}" if h else "") + (".dirty" if dirty else "")
    if a.cmd == "show":
        print(full); return

    P = prefix
    header = f"""/* GÉNÉRÉ par decomp/_kit/version/gen_version.py - ne pas éditer (modifier VERSION puis relancer). */
#ifndef {P}_VERSION_H
#define {P}_VERSION_H
#define {P}_VER_MAJ {maj}
#define {P}_VER_MIN {mnr}
#define {P}_VER_PAT {pat}
#define {P}_VER_MAJOR {maj}
#define {P}_VER_MINOR {mnr}
#define {P}_VER_PATCH {pat}
#define {P}_VER_BUILD {build}
#define {P}_VERSION "{short}"
#define {P}_VERSION_FULL "{full}"
/* alias génériques utilisés par le lanceur commun */
#define KIT_VER_MAJ {maj}
#define KIT_VER_MIN {mnr}
#define KIT_VER_PAT {pat}
#define KIT_VER_BUILD {build}
#define KIT_VERSION "{short}"
#define KIT_VERSION_FULL "{full}"
#define KIT_NAME "{name}"
#define KIT_EXE "{exe}"
#endif
"""
    rc = f"""/* GÉNÉRÉ par gen_version.py - propriétés du .exe (clic droit > Détails) */
#include "version.h"
1 VERSIONINFO
FILEVERSION {P}_VER_MAJ, {P}_VER_MIN, {P}_VER_PAT, {P}_VER_BUILD
PRODUCTVERSION {P}_VER_MAJ, {P}_VER_MIN, {P}_VER_PAT, {P}_VER_BUILD
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040C04B0"
    BEGIN
      VALUE "ProductName", "{name}"
      VALUE "FileDescription", "{desc}"
      VALUE "FileVersion", {P}_VERSION_FULL
      VALUE "ProductVersion", {P}_VERSION_FULL
      VALUE "OriginalFilename", "{exe}"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x040C, 1200
  END
END
"""
    c1 = write_if_changed(os.path.join(out, "version.h"), header)
    c2 = write_if_changed(os.path.join(out, "version.rc"), rc)
    print(f"{name} v{full}" + ("  (version.h mis à jour)" if c1 or c2 else ""))

if __name__ == "__main__":
    main()
