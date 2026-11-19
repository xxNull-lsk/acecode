#!/usr/bin/env bash
# bld.sh - one-click build for ACECode (configure + compile on top of CMakePresets).
#
# Usage: ./bld.sh [options]
#   (no options)          Release build of the acecode TUI binary.
# Options:
#   -d, --debug           Debug build instead of Release.
#   --desktop             Build the desktop preset (includes acecode-desktop).
#   -t, --test            Also build acecode_unit_tests and run ctest.
#   -w, --web             Force rebuild of the embedded web frontend first.
#   --no-web              Skip the web frontend even if web/dist is missing.
#   -p, --preset NAME     Use an explicit CMake configure preset.
#   -j, --jobs N          Parallel build jobs (default: nproc).
#   --clean               Delete the preset's build directory before building.
#   --package             After building, stage and archive like CI
#                         (.github/workflows/package.yml, Unix flow).
#   -h, --help            Show this help.

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$repo_root"

build_type="release"
want_desktop=0
want_test=0
web_mode="auto"     # auto | force | skip
preset_override=""
jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
do_clean=0
want_package=0

die() { printf 'ERROR: %s\n' "$1" >&2; exit 1; }
say() { printf '== %s ==\n' "$1"; }

usage() { sed -n '2,17p' "${BASH_SOURCE[0]}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--debug)     build_type="debug"; shift ;;
        --desktop)      want_desktop=1; shift ;;
        -t|--test)      want_test=1; shift ;;
        -w|--web)       web_mode="force"; shift ;;
        --no-web)       web_mode="skip"; shift ;;
        -p|--preset)    [[ $# -ge 2 ]] || die "--preset requires a name"
                        preset_override="$2"; shift 2 ;;
        -j|--jobs)      [[ $# -ge 2 ]] || die "--jobs requires a number"
                        jobs="$2"; shift 2 ;;
        -j[0-9]*)       jobs="${1#-j}"; shift ;;
        --clean)        do_clean=1; shift ;;
        --package)      want_package=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              die "unknown option: $1 (see --help)" ;;
    esac
done

# ---------------------------------------------------------------------------
# Pick a configure preset from the platform, unless overridden.
# ---------------------------------------------------------------------------
if [[ -n "$preset_override" ]]; then
    preset="$preset_override"
else
    os="$(uname -s)"
    arch="$(uname -m)"
    case "$arch" in
        x86_64|amd64)   arch="x64" ;;
        arm64|aarch64)  arch="arm64" ;;
    esac
    case "$os" in
        Linux)                  base="linux-$arch" ;;
        Darwin)                 base="macos-$arch" ;;
        MINGW*|MSYS*|CYGWIN*)   base="windows-$arch" ;;
        *)                      base="linux-$arch" ;;
    esac
    preset="$base-$build_type"
    [[ $want_desktop -eq 1 ]] && preset="$base-desktop-$build_type"
fi

grep -q "\"name\": \"$preset\"" CMakePresets.json || {
    echo "Preset '$preset' was not found. Available presets:" >&2
    cmake --list-presets >&2 || true
    exit 1
}

build_dir="$repo_root/build/$preset"
binary="$build_dir/acecode"
[[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]] && binary="$binary.exe"

# ---------------------------------------------------------------------------
# Tool prerequisites.
# ---------------------------------------------------------------------------
command -v cmake >/dev/null || die "cmake not found on PATH"
command -v ninja >/dev/null || die "ninja not found on PATH"
if ! cmake --list-presets >/dev/null 2>&1; then
    die "cmake $(cmake --version | head -1 | awk '{print $3}') does not support presets (3.20+ required)"
fi

# Make node/pnpm visible when installed via nvm but PATH was not set up.
for node_bin in "$HOME"/.nvm/versions/node/*/bin; do
    [[ -d "$node_bin" ]] && PATH="$node_bin:$PATH"
done
export PATH

# ---------------------------------------------------------------------------
# vcpkg: use VCPKG_ROOT, then ~/vcpkg; bootstrap a fresh copy as last resort.
# ---------------------------------------------------------------------------
if [[ -z "${VCPKG_ROOT:-}" && -f "$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
    export VCPKG_ROOT="$HOME/vcpkg"
fi
if [[ -z "${VCPKG_ROOT:-}" ]]; then
    say "installing vcpkg into $HOME/vcpkg"
    git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
    "$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
    export VCPKG_ROOT="$HOME/vcpkg"
fi
[[ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]] || \
    die "VCPKG_ROOT does not look like a vcpkg checkout: $VCPKG_ROOT"

# ---------------------------------------------------------------------------
# Submodules and web frontend assets.
# ---------------------------------------------------------------------------
if [[ -f .gitmodules ]] && git rev-parse --git-dir >/dev/null 2>&1; then
    say "submodules"
    git submodule update --init --recursive
fi

if [[ $web_mode == force ]] || { [[ $web_mode == auto ]] && [[ ! -f web/dist/index.html ]]; }; then
    say "building web frontend"
    if command -v pnpm >/dev/null 2>&1; then run_pnpm=(pnpm)
    elif command -v corepack >/dev/null 2>&1; then run_pnpm=(corepack pnpm)
    else die "neither pnpm nor corepack found; install Node.js 22+ or pass --no-web"
    fi
    (
        cd web
        "${run_pnpm[@]}" install
        "${run_pnpm[@]}" test
        "${run_pnpm[@]}" build
    )
else
    say "web frontend: skipped (pass --web to rebuild)"
fi

[[ $do_clean -eq 1 ]] && { say "clean $build_dir"; rm -rf "$build_dir"; }

# ---------------------------------------------------------------------------
# Configure, compile, optionally test.
# ---------------------------------------------------------------------------
say "configure preset: $preset"
# gtest lives behind the vcpkg "tests" feature; presets don't enable it, so the
# flag must be passed explicitly (same as CI and run_cmake_configure.py).
cmake --preset "$preset" -DVCPKG_MANIFEST_FEATURES=tests

say "build (jobs=$jobs)"
cmake --build --preset "$preset" --parallel "$jobs"

if [[ $want_test -eq 1 ]]; then
    say "build tests"
    cmake --build --preset "$preset" --target acecode_unit_tests --parallel "$jobs"
    say "run tests"
    ctest --test-dir "$build_dir" --output-on-failure
fi

# ---------------------------------------------------------------------------
# Optional packaging: mirrors the CI "Package (Unix)" flow in
# .github/workflows/package.yml (component install, sanity checks, strip,
# tar.gz, extracted-package re-verification).
# ---------------------------------------------------------------------------
validate_models_dev_registry() {
    local registry_dir="$1" file file_count
    file_count="$(find "$registry_dir" -type f | wc -l | tr -d '[:space:]')"
    [[ "$file_count" == "3" ]] || \
        die "Expected exactly 3 models.dev files in $registry_dir, found $file_count"
    for file in api.json MANIFEST.json LICENSE; do
        if [[ ! -f "$registry_dir/$file" ]] || ! cmp -s "assets/models_dev/$file" "$registry_dir/$file"; then
            die "Missing or mismatched models.dev package file: $registry_dir/$file"
        fi
    done
}

if [[ $want_package -eq 1 ]]; then
    host_os="$(uname -s)"
    [[ "$host_os" == MINGW* || "$host_os" == MSYS* ]] && \
        die "--package supports Linux/macOS; use the Windows installer flow instead"

    package_id="${preset%-desktop-*}"
    package_id="${package_id%-release}"
    package_id="${package_id%-debug}"
    version="$(grep -m1 'project(acecode VERSION' CMakeLists.txt | sed -E 's/.* VERSION ([0-9.]+).*/\1/')"
    package_dir="$repo_root/dist/acecode-$package_id"

    command -v python3 >/dev/null || die "python3 is required for package verification"
    [[ -f "$binary" ]] || die "Missing terminal executable: $binary"

    # Split debug symbols and strip release binaries (kept next to the build).
    if [[ "$build_type" == release ]]; then
        say "strip ($package_id)"
        if [[ "$host_os" == "Darwin" ]]; then
            command -v dsymutil >/dev/null 2>&1 && dsymutil "$binary" -o "$binary.dSYM"
            strip "$binary"
            if [[ $want_desktop -eq 1 && -f "$build_dir/acecode-desktop" ]]; then
                strip "$build_dir/acecode-desktop"
            fi
        else
            objcopy --only-keep-debug "$binary" "$binary.debug"
            strip --strip-unneeded "$binary"
            objcopy --add-gnu-debuglink="$binary.debug" "$binary"
            if [[ $want_desktop -eq 1 && -f "$build_dir/acecode-desktop" ]]; then
                objcopy --only-keep-debug "$build_dir/acecode-desktop" "$build_dir/acecode-desktop.debug"
                strip --strip-unneeded "$build_dir/acecode-desktop"
                objcopy --add-gnu-debuglink="$build_dir/acecode-desktop.debug" "$build_dir/acecode-desktop"
            fi
        fi
    else
        echo "NOTE: debug build is packaged unstripped" >&2
    fi

    say "stage dist/acecode-$package_id"
    rm -rf "$package_dir"
    mkdir -p "$package_dir"
    cp "$binary" "$package_dir/"
    cp README.md README_CN.md "$package_dir/"

    install_component() {
        cmake --install "$build_dir" --prefix "$package_dir" --component "$1" >/dev/null
    }
    install_component models_dev_registry
    install_component default_seed_bundle
    install_component whatsapp_channel
    if [[ "$host_os" == "Darwin" ]]; then
        install_component computer_use_runtime
        install_component ios_simulator_runtime
    fi

    if [[ $want_desktop -eq 1 ]]; then
        if [[ "$host_os" == "Darwin" ]]; then
            app_bundle="$build_dir/ACECode.app"
            [[ -d "$app_bundle" ]] || die "Missing desktop app bundle: $app_bundle"
            cp -R "$app_bundle" "$package_dir/"
        else
            [[ -f "$build_dir/acecode-desktop" ]] || die "Missing desktop executable: $build_dir/acecode-desktop"
            cp "$build_dir/acecode-desktop" "$package_dir/"
            cp "$build_dir/acecode-logo.png" "$package_dir/"
        fi
    fi

    validate_models_dev_registry "$package_dir/share/acecode/models_dev"
    python3 scripts/verify_seed_bundle.py \
        --source assets/seed \
        --packaged "$package_dir/share/acecode/seed"

    say "archive dist/acecode-$package_id.tar.gz"
    mkdir -p "$repo_root/dist"
    tar -C "$repo_root/dist" -czf "$repo_root/dist/acecode-$package_id.tar.gz" "acecode-$package_id"

    # Re-verify from the archive, the same way CI does.
    verify_root="$(mktemp -d)"
    tar -xzf "$repo_root/dist/acecode-$package_id.tar.gz" -C "$verify_root"
    extracted="$verify_root/acecode-$package_id"
    validate_models_dev_registry "$extracted/share/acecode/models_dev"
    python3 scripts/verify_seed_bundle.py \
        --source assets/seed \
        --packaged "$extracted/share/acecode/seed"
    [[ -x "$extracted/$(basename "$binary")" ]] || die "Extracted package lost the executable bit: $extracted"
    if [[ $want_desktop -eq 1 ]]; then
        if [[ "$host_os" == "Darwin" ]]; then
            [[ -d "$extracted/ACECode.app" ]] || die "Extracted package lost ACECode.app"
        else
            [[ -x "$extracted/acecode-desktop" ]] || die "Extracted package lost acecode-desktop"
        fi
    fi
    rm -rf "$verify_root"

    package_archive="$repo_root/dist/acecode-$package_id.tar.gz"
fi

printf '\nDONE in %ss\n' "$SECONDS"
printf 'BINARY=%s\n' "$binary"
if [[ -n "${package_archive:-}" ]]; then
    printf 'PACKAGE=%s\n' "$package_archive"
    printf 'VERSION=%s\n' "$version"
fi
