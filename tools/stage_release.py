#!/usr/bin/env python3
"""Stage the release payload: a folder, a zip of it, and the loose files.

    python3 tools/stage_release.py --bootloader build/bootloader.bin \
                                   --out build/dist

WHAT COMES OUT

    build/dist/gbflash-bootloader.zip     one download, extracts to a folder
    build/dist/<each file>                the same files loose
    build/dist/SHA256SUMS                 the loose files AND the zip

Both shapes are published. The zip is one click and one extract on every
platform a downloader might be on; the loose files are there for anyone who
wants a single script without the rest, and for `sha256sum -c SHA256SUMS`.
The zip carries its own SHA256SUMS covering just its contents, so verifying
works after extracting too.

The output directory is deliberately FLAT -- the payload is assembled in a
temporary directory and only the zip survives. .github/workflows/release.yml
does `cp build/dist/* somewhere/`, which fails the job under `bash -e` the
moment a subdirectory appears in there.

THE STAMP

install.py ships with `BL_SHA256 = None`, meaning "built here, nothing to
compare against". This tool rewrites that line in the STAGED copy with the
digest of the bootloader.bin staged beside it, so a release is self-consistent
whatever compiler built it, and install.py can tell a downloader whether the
bootloader.bin next to it is the one that was published with it.

That constant used to be maintained by hand. The machine writing the constant
was never the machine building the binary, so a release would have denounced
its own bootloader.bin to every downloader. The stamp is asserted below rather
than assumed: if the substitution does not apply, or the staged install.py does
not then report the digest of the staged binary, this exits non-zero rather
than publishing a payload whose safety check silently does nothing.
"""

import argparse
import hashlib
import os
import re
import shutil
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Staged flat, in the order a reader should meet them: the front door first.
PAYLOAD = [
    ("install.py", "install.py"),
    ("requirements.txt", "requirements.txt"),
    ("tools/build_composite.py", "build_composite.py"),
    ("docs/backup-codeflash.py", "backup-codeflash.py"),
    ("docs/check-bootloader-region.py", "check-bootloader-region.py"),
    ("LICENSE", "LICENSE"),
    ("tools/payload-README.txt", "README.txt"),
]

PLACEHOLDER_URL = "https://github.com/OWNER/REPO"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def stamp_install_py(path, digest):
    """Rewrite BL_SHA256 in the staged install.py. Loud on failure."""
    with open(path, "r") as f:
        src = f.read()
    new, n = re.subn(r"^BL_SHA256 = None$", 'BL_SHA256 = "%s"' % digest,
                     src, count=1, flags=re.M)
    if n != 1:
        sys.exit("stage_release: could not stamp BL_SHA256 in %s -- expected "
                 "exactly one line reading `BL_SHA256 = None`, found %d.\n"
                 "Refusing to stage a payload whose bootloader check would "
                 "silently do nothing." % (path, n))
    with open(path, "w") as f:
        f.write(new)


def read_stamped_constant(path):
    """What the staged install.py actually says, read the way Python will.

    Executing the module is not an option -- it has a __main__ guard but also a
    large import surface -- so the constant is parsed out of the source. That is
    the same text the interpreter will see.
    """
    with open(path, "r") as f:
        for line in f:
            m = re.match(r'^BL_SHA256 = "([0-9a-f]{64})"$', line.strip())
            if m:
                return m.group(1)
    return None


def write_sha256sums(directory, names):
    lines = ["%s  %s\n" % (sha256_file(os.path.join(directory, n)), n)
             for n in names]
    with open(os.path.join(directory, "SHA256SUMS"), "w") as f:
        f.writelines(lines)
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Stage the release payload (folder + zip + loose files).")
    ap.add_argument("--bootloader", required=True,
                    help="the built bootloader.bin to publish")
    ap.add_argument("--out", required=True,
                    help="staging directory; emptied first")
    ap.add_argument("--name", default="gbflash-bootloader",
                    help="payload folder and zip basename")
    ap.add_argument("--repo-url", default=PLACEHOLDER_URL,
                    help="substituted for @REPO_URL@ in README.txt")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.bootloader):
        sys.exit("stage_release: no such file: %s\nRun `make` first."
                 % args.bootloader)

    out = os.path.abspath(args.out)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    work = tempfile.mkdtemp(prefix="gbflash-stage-")
    try:
        return stage(args, out, os.path.join(work, args.name))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def stage(args, out, payload):
    os.makedirs(payload)

    # ---- copy ------------------------------------------------------------
    shutil.copy2(args.bootloader, os.path.join(payload, "bootloader.bin"))
    for src, dst in PAYLOAD:
        full = os.path.join(ROOT, src)
        if not os.path.isfile(full):
            sys.exit("stage_release: missing %s" % src)
        shutil.copy2(full, os.path.join(payload, dst))

    # ---- the repo URL, so a flat payload can point somewhere -------------
    readme = os.path.join(payload, "README.txt")
    with open(readme, "r") as f:
        text = f.read()
    if "@REPO_URL@" not in text:
        sys.exit("stage_release: @REPO_URL@ is gone from payload-README.txt; "
                 "the download would ship with no link back to the docs.")
    with open(readme, "w") as f:
        f.write(text.replace("@REPO_URL@", args.repo_url))

    # ---- stamp, then prove the stamp took --------------------------------
    digest = sha256_file(os.path.join(payload, "bootloader.bin"))
    staged_install = os.path.join(payload, "install.py")
    stamp_install_py(staged_install, digest)
    got = read_stamped_constant(staged_install)
    if got != digest:
        sys.exit("stage_release: the staged install.py reports BL_SHA256=%r "
                 "but the staged bootloader.bin is %s. Not publishing."
                 % (got, digest))

    # ---- checksums inside the payload, for after extraction --------------
    names = sorted(os.listdir(payload))
    write_sha256sums(payload, names)

    # ---- the zip ---------------------------------------------------------
    zip_path = os.path.join(out, args.name + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for n in sorted(os.listdir(payload)):
            z.write(os.path.join(payload, n), os.path.join(args.name, n))

    # ---- the same files loose --------------------------------------------
    # SHA256SUMS COVERS THE LOOSE FILES ONLY.  Listing the zip in it breaks the
    # loose-file route the release body offers: `sha256sum -c SHA256SUMS` then
    # ends "gbflash-bootloader.zip: FAILED open or read", exit 1, as the first
    # command someone runs before erasing their CodeFlash.  The zip carries its
    # own one-line digest file instead, and each shape verifies cleanly on its
    # own terms.
    loose = [n for n in names if n != "SHA256SUMS"]
    for n in loose:
        shutil.copy2(os.path.join(payload, n), os.path.join(out, n))
    lines = write_sha256sums(out, sorted(loose))

    zip_name = os.path.basename(zip_path)
    zip_sum = "%s  %s\n" % (sha256_file(zip_path), zip_name)
    with open(zip_path + ".sha256", "w") as f:
        f.write(zip_sum)
    lines = lines + [zip_sum]

    print("  DIST    %s" % out)
    for line in lines:
        print("          " + line.rstrip())
    print()
    print("  one download:  %s" % os.path.basename(zip_path))
    print("  bootloader.bin is NOT flashable on its own -- build_composite.py")
    print("  builds the flashable image from the user's own backup.")
    print("  The guided install is:  python3 install.py")
    print("  Rehearse it with no hardware:  python3 install.py --dry-run")
    if args.repo_url == PLACEHOLDER_URL:
        print()
        print("  NOTE: README.txt carries the placeholder repository URL.")
        print("  Pass --repo-url to put the real one in.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
