"""Makes the macOS app standalone (runs on a Mac without Homebrew): a copy of the built app in build/native/dist with every
non-system library it loads (SDL3, FFmpeg's libavcodec and libavutil and what they load in turn) copied into Contents/Frameworks,
each one's install name @rpath/<name> and every reference to it rewritten to that, the executable given the rpath
@executable_path/../Frameworks, their packages' licence files in Contents/Resources/licenses, and the whole bundle signed ad hoc (codesign -s -; Apple Silicon won't run unsigned code).
FFmpeg is the app's own LGPL build (native/tools/build_ffmpeg.sh) when CMake linked it, with native/third_party/ffmpeg's README
and licence in Contents/Resources/licenses/ffmpeg; a Homebrew FFmpeg (GPL 3) is bundled with a warning.

    python native/tools/bundle_app.py [--app PATH] [--x86-64-app PATH] [--out DIR] [--zip]

--x86-64-app makes a universal app: the x86-64 build (cmake -DCMAKE_OSX_ARCHITECTURES=x86_64, native/NATIVE.md) made standalone
the same way, and its executable and libraries joined to --app's (lipo) in one bundle, which Apple silicon and Intel Macs both run.
It then checks: no library outside the system's left in any Mach-O of the bundle (otool -L), and the signature verifies. --zip also
writes DIR/Crash-Twinsanity-macOS.zip (ditto, keeping the signature).
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "build/native/cmake/Crash Twinsanity.app"
OUT = ROOT / "build/native/dist"
SYSTEM = ("/usr/lib/", "/System/")
# The app's own FFmpeg (native/tools/build_ffmpeg.sh's builds) and what goes with it
OWN_FFMPEG = ROOT / "build/native/ffmpeg"
FFMPEG_LICENCES = ROOT / "native/third_party/ffmpeg"


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def dependencies(binary):
    """The libraries a Mach-O loads (otool -L), its own install name left out."""
    lines = run("otool", "-L", str(binary)).splitlines()[1:]
    paths = [line.strip().split(" (compatibility")[0] for line in lines]
    own = run("otool", "-D", str(binary)).splitlines()[1:]
    return [p for p in paths if p not in own]


def resolve(reference, loader, rpaths):
    """A reference's file: absolute, @loader_path or @rpath (the loader's rpaths, then Homebrew's lib)."""
    if reference.startswith("@loader_path/"):
        return (Path(loader).parent / reference[len("@loader_path/"):]).resolve()
    if reference.startswith("@rpath/"):
        name = reference[len("@rpath/"):]
        for rpath in rpaths + ["/opt/homebrew/lib", "/usr/local/lib"]:
            candidate = Path(rpath.replace("@loader_path", str(Path(loader).parent))) / name
            if candidate.exists():
                return candidate.resolve()
        return None
    return Path(reference).resolve()


def rpaths_of(binary):
    text = run("otool", "-l", str(binary))
    return re.findall(r"cmd LC_RPATH\n\s+cmdsize \d+\n\s+path (\S+)", text)


def standalone(source, out):
    """A copy of the app in out with its libraries in Contents/Frameworks and their licences: the app, its executable,
    {name: library} and the licences' packages."""
    out.mkdir(parents=True, exist_ok=True)
    app = out / source.name
    if app.exists():
        shutil.rmtree(app)
    shutil.copytree(source, app, symlinks=True)
    executable = app / "Contents/MacOS" / source.stem
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(exist_ok=True)

    # Every non-system library reachable from the executable, copied once by its file name
    copied = {}
    origins = {}
    queue = [executable]
    while queue:
        binary = queue.pop()
        rpaths = rpaths_of(binary)
        for reference in dependencies(binary):
            if reference.startswith(SYSTEM):
                continue
            path = resolve(reference, binary, rpaths)
            if path is None or not path.exists():
                sys.exit(f"{binary.name}: can't find {reference}")
            name = Path(reference).name
            if name not in copied:
                target = frameworks / name
                shutil.copy2(path, target)
                target.chmod(0o755)
                copied[name] = target
                origins[name] = path
                queue.append(target)
            run("install_name_tool", "-change", reference, f"@rpath/{name}", str(binary))

    for name, target in copied.items():
        run("install_name_tool", "-id", f"@rpath/{name}", str(target))
        for rpath in rpaths_of(target):
            run("install_name_tool", "-delete_rpath", rpath, str(target))
        if "@loader_path" not in rpaths_of(target):
            run("install_name_tool", "-add_rpath", "@loader_path", str(target))
    for rpath in rpaths_of(executable):
        if not rpath.startswith("@"):
            run("install_name_tool", "-delete_rpath", rpath, str(executable))
    if "@executable_path/../Frameworks" not in rpaths_of(executable):
        run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", str(executable))

    # Each library's licence files (its Homebrew package's LICENSE*, COPYING*), in Contents/Resources/licenses/<package>
    licenses = app / "Contents/Resources/licenses"
    packages = set()
    for name, origin in origins.items():
        if origin.is_relative_to(OWN_FFMPEG.resolve()):
            packages.add("ffmpeg (LGPL 2.1, the app's own build)")
            for licence in (FFMPEG_LICENCES / "README.md", FFMPEG_LICENCES / "COPYING.LGPLv2.1"):
                target = licenses / "ffmpeg" / licence.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(licence, target)
            continue
        if name.startswith(("libavcodec", "libavutil")):
            print(f"warning: {name} is {origin}, not the app's LGPL build (native/tools/build_ffmpeg.sh): a Homebrew FFmpeg is "
                  "GPL 3, and the app carrying it can't be shared under the LGPL", file=sys.stderr)
        source_path = None
        for prefix in ("/opt/homebrew/lib", "/usr/local/lib"):
            candidate = Path(prefix) / name
            if candidate.exists():
                source_path = candidate.resolve()
        if source_path is None or "Cellar" not in source_path.parts:
            continue
        cellar = source_path.parts.index("Cellar")
        package_root = Path(*source_path.parts[:cellar + 3])
        packages.add(package_root.parts[cellar + 1])
        for licence in list(package_root.glob("LICENSE*")) + list(package_root.glob("COPYING*")) + list(package_root.glob("LICENCE*")):
            target = licenses / package_root.parts[cellar + 1] / licence.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(licence, target)

    return app, executable, copied, packages


def join(executable, copied, other_executable, other_copied):
    """The other build's executable and libraries joined to these (lipo): one universal file each."""
    if set(copied) != set(other_copied):
        sys.exit(f"the two builds load different libraries: {', '.join(sorted(set(copied) ^ set(other_copied)))}")
    for mine, theirs in [(executable, other_executable)] + [(copied[name], other_copied[name]) for name in copied]:
        joined = mine.with_name(mine.name + ".universal")
        run("lipo", "-create", str(mine), str(theirs), "-output", str(joined))
        joined.chmod(0o755)
        joined.replace(mine)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", default=str(APP))
    parser.add_argument("--x86-64-app", help="an x86-64 build of the app, joined to --app's in a universal app")
    parser.add_argument("--out", default=str(OUT))
    parser.add_argument("--zip", action="store_true")
    arguments = parser.parse_args()
    app, executable, copied, packages = standalone(Path(arguments.app), Path(arguments.out))
    if arguments.x86_64_app:
        with tempfile.TemporaryDirectory() as folder:
            _, other_executable, other_copied, other_packages = standalone(Path(arguments.x86_64_app), Path(folder))
            join(executable, copied, other_executable, other_copied)
            packages |= other_packages

    # Signed ad hoc: the libraries first, then the bundle
    for target in copied.values():
        run("codesign", "--force", "--sign", "-", str(target))
    run("codesign", "--force", "--sign", "-", str(app))

    # Checks: nothing outside the system's left, the signature good
    bad = []
    for binary in [executable, *copied.values()]:
        for reference in dependencies(binary):
            if not reference.startswith(SYSTEM) and not reference.startswith("@rpath/"):
                bad.append(f"{binary.name}: {reference}")
    if bad:
        sys.exit("still outside the bundle:\n" + "\n".join(bad))
    run("codesign", "--verify", "--deep", "--strict", str(app))
    print(f"{app} ({run('lipo', '-archs', str(executable)).strip()}): {len(copied)} libraries in Contents/Frameworks: {', '.join(sorted(copied))}")
    print(f"licences of: {', '.join(sorted(packages))}")
    if arguments.zip:
        archive = Path(arguments.out) / "Crash-Twinsanity-macOS.zip"
        archive.unlink(missing_ok=True)
        run("ditto", "-c", "-k", "--keepParent", str(app), str(archive))
        print(archive)


if __name__ == "__main__":
    main()
