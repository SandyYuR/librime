#!/usr/bin/env python3
"""Build the complete Android SDK from pinned sources, without prebuilt inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
LOCK = HERE / "sources.lock.json"
LIBRARIES = {
    "librime": "librime.a", "glog": "libglog.a", "leveldb": "libleveldb.a",
    "lua": "liblua_static.a", "marisa": "libmarisa.a", "opencc": "libopencc.a",
    "yaml-cpp": "libyaml-cpp.a",
}


def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def apply_patch(target, patch):
    # An extracted source directory may be nested below this repository's
    # build/. Without a discovery ceiling, git apply treats it as a subdirectory
    # of the outer checkout and silently skips patches whose paths lack that
    # prefix. Apply against the extracted tree in both local and CI layouts.
    environment = dict(os.environ, GIT_CEILING_DIRECTORIES=str(target.parent))
    run("git", "apply", "--check", patch, cwd=target, env=environment)
    run("git", "apply", patch, cwd=target, env=environment)


def download(item, cache):
    target = cache / item["sha256"]
    if target.exists() and sha256(target) == item["sha256"]:
        return target
    for attempt in range(4):
        try:
            with urllib.request.urlopen(item["url"], timeout=120) as response:
                with tempfile.NamedTemporaryFile(dir=cache, delete=False) as output:
                    temporary = Path(output.name)
                    shutil.copyfileobj(response, output)
            if sha256(temporary) != item["sha256"]:
                raise ValueError(f"Source checksum mismatch: {item['url']}")
            temporary.replace(target)
            return target
        except Exception:
            if "temporary" in locals():
                temporary.unlink(missing_ok=True)
            if attempt == 3:
                raise
            time.sleep(2 ** attempt)


def prepare_source(name, item, sources, cache, patches=True):
    target = sources / name
    key = hashlib.sha256(json.dumps([item, patches], sort_keys=True).encode()).hexdigest()
    if (target / ".android-source").exists():
        if (target / ".android-source").read_text() == key:
            return target
        shutil.rmtree(target)
    with tempfile.TemporaryDirectory(dir=sources) as temporary:
        with tarfile.open(download(item, cache)) as archive:
            archive.extractall(temporary, filter="data")
        roots = list(Path(temporary).iterdir())
        if len(roots) != 1 or not roots[0].is_dir():
            raise ValueError(f"Expected one source root: {name}")
        shutil.move(roots[0], target)
    if patches:
        for patch in item["patches"]:
            # OpenCC uses separate source trees for host data and Android library.
            if patch["file"] == "opencc-host.patch":
                continue
            path = HERE / "patches" / patch["file"]
            if sha256(path) != patch["sha256"]:
                raise ValueError(f"Patch checksum mismatch: {path.name}")
            apply_patch(target, path)
    (target / ".android-source").write_text(key)
    return target


def cmake_build(cmake, source, build, prefix, jobs, flags=(), targets=(), install=True):
    # Source archives have no Git metadata. Never let a dependency's version
    # detection discover the enclosing librime checkout in CI's build/ tree.
    environment = dict(os.environ, GIT_CEILING_DIRECTORIES=str(source.parent))
    run(cmake, "-S", source, "-B", build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib", *flags,
        env=environment)
    run(cmake, "--build", build, "--parallel", jobs,
        *(["--target", *targets] if targets else []), env=environment)
    if install:
        run(cmake, "--install", build, env=environment)


def verify_opencc_version(build, expected):
    versions = [line.split(":", 1)[1].strip() for line in (build / "opencc.pc").read_text().splitlines()
                if line.startswith("Version:")]
    if versions != [expected]:
        raise ValueError(f"OpenCC version does not match its locked source: {versions}, expected {expected}")


def prepare_boost_headers(source, include, source_sha256):
    stamp = include / ".android-source"
    if stamp.exists() and stamp.read_text() == source_sha256:
        return
    if include.exists():
        shutil.rmtree(include)
    include.mkdir(parents=True)
    for directory in sorted((source / "libs").rglob("include/boost")):
        shutil.copytree(directory, include / "boost", dirs_exist_ok=True)
    stamp.write_text(source_sha256)


def copy_licenses(sources, product):
    destination = product / "licenses"
    destination.mkdir(exist_ok=True)
    for name, source in sources.items():
        directory = destination / name
        for path in source.iterdir():
            if path.is_file() and path.name.lower().startswith(("license", "copying", "notice", "copyright")):
                directory.mkdir(exist_ok=True)
                shutil.copy2(path, directory / path.name)
        if name == "lua":
            directory.mkdir(exist_ok=True)
            shutil.copy2(source / "doc/readme.html", directory / "Lua-license.html")
    shutil.copy2(ROOT / "LICENSE", destination / "librime-LICENSE")


def build(args):
    lock = json.loads(LOCK.read_text())
    if args.abi not in lock["abis"]:
        raise ValueError("Unsupported ABI")
    # A release must describe committed source, including its build recipe.
    run("git", "-C", ROOT, "diff", "--exit-code", "HEAD", "--")
    ndk = Path(os.environ["ANDROID_NDK_ROOT"]).resolve()
    if f"Pkg.Revision = {lock['ndk']}" not in (ndk / "source.properties").read_text():
        raise ValueError("ANDROID_NDK_ROOT does not match the locked NDK")
    cmake = os.environ.get("CMAKE_BIN", "cmake")
    version = subprocess.check_output([cmake, "--version"], text=True).splitlines()[0].split()[-1]
    if version != lock["cmake"]:
        raise ValueError(f"Expected CMake {lock['cmake']}, got {version}")
    work = args.work.resolve()
    cache = args.cache.resolve()
    sources = work / "sources"
    for directory in (cache, sources):
        directory.mkdir(parents=True, exist_ok=True)
    prepared = {
        name: prepare_source(name, item, sources, cache)
        for name, item in lock["sources"].items()
    }
    commit = git("rev-parse", "HEAD")
    core = sources / "librime"
    if not (core / ".android-source").exists() or (core / ".android-source").read_text() != commit:
        if core.exists():
            shutil.rmtree(core)
        core.mkdir()
        with tempfile.TemporaryFile() as output:
            run("git", "-C", ROOT, "archive", "HEAD", stdout=output)
            output.seek(0)
            with tarfile.open(fileobj=output) as archive:
                archive.extractall(core, filter="data")
        (core / ".android-source").write_text(commit)
        for name, directory in (("librime-lua", "lua"), ("librime-octagram", "octagram"), ("librime-predict", "predict")):
            (core / "plugins" / directory).symlink_to(prepared[name], target_is_directory=True)
    product = work / "product"
    product.mkdir(exist_ok=True)
    copy_licenses(prepared, product)
    # Boost is header-only on Android. Assemble its modular distribution into
    # the conventional include tree; no host Boost or Android Boost binary.
    boost = work / "boost/include"
    prepare_boost_headers(prepared["boost"], boost, lock["sources"]["boost"]["sha256"])
    common = [
        f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake",
        f"-DANDROID_ABI={args.abi}", f"-DANDROID_PLATFORM={lock['platform']}",
        "-DANDROID_STL=c++_shared", "-DBUILD_SHARED_LIBS=OFF",
        f"-DCMAKE_C_FLAGS=-ffile-prefix-map={work}=.",
        f"-DCMAKE_CXX_FLAGS=-ffile-prefix-map={work}=. -DBOOST_ALL_NO_EMBEDDED_GDB_SCRIPTS",
    ]
    flags = {
        "glog": ["-DWITH_GFLAGS=OFF", "-DWITH_UNWIND=OFF", "-DBUILD_TESTING=OFF"],
        "yaml-cpp": ["-DYAML_CPP_BUILD_CONTRIB=OFF", "-DYAML_CPP_BUILD_TESTS=OFF", "-DYAML_CPP_BUILD_TOOLS=OFF"],
        "leveldb": ["-DLEVELDB_BUILD_TESTS=OFF", "-DLEVELDB_BUILD_BENCHMARKS=OFF", "-DLEVELDB_INSTALL=ON"],
        "marisa": [],
        "lua": [],
        "opencc": ["-DSHARE_INSTALL_PREFIX=share", "-DINCLUDE_INSTALL_DIR=include", "-DSYSCONF_INSTALL_DIR=etc", "-DLIB_INSTALL_DIR=lib", "-DBUILD_DOCUMENTATION=OFF", "-DBUILD_PYTHON=OFF", "-DBUILD_OPENCC_JIEBA_PLUGIN=OFF", "-DENABLE_GTEST=OFF", "-DENABLE_BENCHMARK=OFF", "-DUSE_SYSTEM_MARISA=ON", f"-DMarisa_DIR={product}/marisa/{args.abi}/lib/cmake/Marisa", "-DUSE_SYSTEM_PYBIND11=OFF", "-DUSE_SYSTEM_RAPIDJSON=OFF", "-DUSE_SYSTEM_TCLAP=OFF"],
    }
    for name in ("glog", "yaml-cpp", "leveldb", "marisa", "lua", "opencc"):
        cmake_build(cmake, prepared[name], work / "cmake" / args.abi / name,
                    product / name / args.abi, args.jobs, common + flags[name])
        if name == "opencc":
            verify_opencc_version(work / "cmake" / args.abi / name, lock["sources"]["opencc"]["version"])
    # Generate OpenCC dictionaries using native host tools, never Android
    # executables or files copied out of the retired artifact repository.
    host = work / "host"
    cmake_build(cmake, prepared["marisa"], work / "cmake/host/marisa", host,
                args.jobs, ["-DBUILD_SHARED_LIBS=OFF"])
    host_opencc = prepare_source("opencc-host", lock["sources"]["opencc"], sources, cache, patches=False)
    patch = next(p for p in lock["sources"]["opencc"]["patches"] if p["file"] == "opencc-host.patch")
    patch_path = HERE / "patches" / patch["file"]
    if sha256(patch_path) != patch["sha256"]:
        raise ValueError("OpenCC host patch checksum mismatch")
    if not (host_opencc / ".host-patched").exists():
        apply_patch(host_opencc, patch_path)
        (host_opencc / ".host-patched").touch()
    cmake_build(cmake, host_opencc, work / "cmake/host/opencc", product / "opencc",
                args.jobs, [f"-DCMAKE_PREFIX_PATH={host}", "-DBUILD_SHARED_LIBS=OFF", "-DENABLE_GTEST=OFF", "-DBUILD_OPENCC_JIEBA_PLUGIN=OFF"],
                targets=["opencc_dict", "Dictionaries"], install=False)
    verify_opencc_version(work / "cmake/host/opencc", lock["sources"]["opencc"]["version"])
    run(cmake, "--install", work / "cmake/host/opencc", "--component", "data")
    roots = ";".join([str(work / "boost"), *(str(product / name / args.abi) for name in flags)])
    cmake_build(cmake, core, work / "cmake" / args.abi / "librime", product / "librime" / args.abi,
                args.jobs, common + ["-DBUILD_STATIC=ON", "-DBUILD_MERGED_PLUGINS=ON", "-DBUILD_TEST=OFF", "-DBUILD_TOOLS=OFF", "-DALSO_LOG_TO_STDERR=ON", f"-DCMAKE_FIND_ROOT_PATH={roots}", f"-DBoost_INCLUDE_DIR={boost}", "-DBoost_NO_BOOST_CMAKE=ON"], targets=["rime-static"])
    toolchain = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    for name, library in LIBRARIES.items():
        run(toolchain / "llvm-strip", "--strip-unneeded", product / name / args.abi / "lib" / library)
    # A real final link catches dependency omissions and incompatible objects.
    probe = work / f"link-{args.abi}.cc"
    probe.write_text('extern "C" void* rime_get_api();\nextern "C" void* sdk_api() { return rime_get_api(); }\n')
    target = {"armeabi-v7a": "armv7a-linux-androideabi", "arm64-v8a": "aarch64-linux-android", "x86": "i686-linux-android", "x86_64": "x86_64-linux-android"}[args.abi]
    run(toolchain / f"{target}{lock['platform']}-clang++", "-shared", "-fPIC", probe,
        "-Wl,--no-undefined", "-Wl,--whole-archive", product / "librime" / args.abi / "lib/librime.a", "-Wl,--no-whole-archive",
        *(product / name / args.abi / "lib" / lib for name, lib in LIBRARIES.items() if name != "librime"),
        "-llog", "-landroid", "-o", work / f"link-{args.abi}.so")
    downstream = json.loads((ROOT / "downstream/manifest.json").read_text())
    manifest = {
        "schema_version": 2, "librime_repository": downstream["repository"],
        "librime_commit": commit, "source_tree": git("rev-parse", "HEAD^{tree}"),
        "upstream_commit": downstream["upstream_commit"], "abi": args.abi,
        "ndk": lock["ndk"], "cmake": lock["cmake"], "platform": lock["platform"],
        "sources_lock_sha256": sha256(LOCK), "sources": lock["sources"],
        "archive_sha256": sha256(product / "librime" / args.abi / "lib/librime.a"),
        "rime_api_sha256": sha256(product / "librime" / args.abi / "include/rime_api.h"),
        "dependency_archives_sha256": {name: sha256(product / name / args.abi / "lib" / lib) for name, lib in LIBRARIES.items() if name != "librime"},
        "link_smoke_test": "passed", "core_patch_mode": "committed-source",
    }
    output = product / "librime" / args.abi / "share/rime/source-manifest.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Built {args.abi} from {commit}: {product}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--abi", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--cache", type=Path, default=Path.home() / ".cache/rime-android-sources")
    parser.add_argument("--jobs", type=int, default=2)
    build(parser.parse_args())
