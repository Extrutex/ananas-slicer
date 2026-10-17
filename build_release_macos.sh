#!/bin/bash

set -e
set -o pipefail
SECONDS=0

while getopts ":dpa:snt:xbc:i:j:Tuh" opt; do
  case "${opt}" in
    d )
        export BUILD_TARGET="deps"
        ;;
    p )
        export PACK_DEPS="1"
        ;;
    a )
        export ARCH="$OPTARG"
        ;;
    s )
        export BUILD_TARGET="slicer"
        ;;
    n )
        export NIGHTLY_BUILD="1"
        ;;
    t )
        export OSX_DEPLOYMENT_TARGET="$OPTARG"
        ;;
    x )
        export SLICER_CMAKE_GENERATOR="Ninja Multi-Config"
        export SLICER_BUILD_TARGET="all"
        export DEPS_CMAKE_GENERATOR="Ninja"
        ;;
    b )
        export BUILD_ONLY="1"
        ;;
    c )
        export BUILD_CONFIG="$OPTARG"
        ;;
    i )
        export CMAKE_IGNORE_PREFIX_PATH="${CMAKE_IGNORE_PREFIX_PATH:+$CMAKE_IGNORE_PREFIX_PATH;}$OPTARG"
        ;;
    j )
        export CMAKE_BUILD_PARALLEL_LEVEL="$OPTARG"
        ;;
    T )
        export BUILD_TESTS="1"
        ;;
    u )
        export BUILD_TARGET="universal"
        ;;
    h ) echo "Usage: ./build_release_macos.sh [-d]"
        echo "   -d: Build deps only"
        echo "   -a: Set ARCHITECTURE (arm64 or x86_64 or universal)"
        echo "   -s: Build slicer only"
        echo "   -u: Build universal app only (requires existing arm64 and x86_64 app bundles)"
        echo "   -n: Nightly build"
        echo "   -t: Specify minimum version of the target platform, default is 12.0"
        echo "   -x: Use Ninja Multi-Config CMake generator, default is Xcode"
        echo "   -b: Build without reconfiguring CMake"
        echo "   -c: Set CMake build configuration, default is Release"
        echo "   -i: Add a prefix to ignore during CMake dependency discovery (repeatable), defaults to /opt/local:/usr/local:/opt/homebrew"
        echo "   -j: Set the number of parallel build jobs (CMAKE_BUILD_PARALLEL_LEVEL)"
        echo "   -T: Build and run tests (set ORCA_TESTS_BUILD_ONLY=1 to build without running)"
        echo
        echo "Environment:"
        echo "   KLIPSLICE_OSX_SYSROOT: SDK path or name (e.g. macosx26.5) for deps and slicer configure;"
        echo "                          default for the slicer is the SDK recorded by the deps build"
        echo "                          (deps/build/<arch>/OrcaSlicer_dep/klipslice-sdk.txt, then CMakeCache.txt)"
        echo "   KLIPSLICE_BUILD_DIR:   slicer build directory (absolute or relative to the repository),"
        echo "                          default build/<arch>; not allowed with -a universal or -u"
        exit 0
        ;;
    * )
        ;;
  esac
done

# Set defaults

if [ -z "$ARCH" ]; then
    ARCH="$(uname -m)"
    export ARCH
fi

if [ -z "$BUILD_CONFIG" ]; then
  export BUILD_CONFIG="Release"
fi

if [ -z "$BUILD_TARGET" ]; then
  export BUILD_TARGET="all"
fi

if [ -z "$SLICER_CMAKE_GENERATOR" ]; then
  export SLICER_CMAKE_GENERATOR="Xcode"
fi

if [ -z "$SLICER_BUILD_TARGET" ]; then
  export SLICER_BUILD_TARGET="ALL_BUILD"
fi

if [ -z "$DEPS_CMAKE_GENERATOR" ]; then
  export DEPS_CMAKE_GENERATOR="Unix Makefiles"
fi

if [ -z "$OSX_DEPLOYMENT_TARGET" ]; then
  export OSX_DEPLOYMENT_TARGET="12.0"
fi

if [ -z "$CMAKE_IGNORE_PREFIX_PATH" ]; then
  export CMAKE_IGNORE_PREFIX_PATH="/opt/local:/usr/local:/opt/homebrew"
fi

CMAKE_VERSION=$(cmake --version | head -1 | sed 's/[^0-9]*\([0-9]*\).*/\1/')
if [ "$CMAKE_VERSION" -ge 4 ] 2>/dev/null; then
  export CMAKE_POLICY_VERSION_MINIMUM=3.5
  export CMAKE_POLICY_COMPAT="-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
  echo "Detected CMake 4.x, adding compatibility flag (env + cmake arg)"
else
  export CMAKE_POLICY_COMPAT=""
fi

echo "Build params:"
echo " - ARCH: $ARCH"
echo " - BUILD_CONFIG: $BUILD_CONFIG"
echo " - BUILD_TARGET: $BUILD_TARGET"
echo " - CMAKE_GENERATOR: $SLICER_CMAKE_GENERATOR for Slicer, $DEPS_CMAKE_GENERATOR for deps"
echo " - OSX_DEPLOYMENT_TARGET: $OSX_DEPLOYMENT_TARGET"
echo " - CMAKE_IGNORE_PREFIX_PATH: $CMAKE_IGNORE_PREFIX_PATH"
echo

# if which -s brew; then
# 	brew --prefix libiconv
# 	brew --prefix zstd
# 	export LIBRARY_PATH=$LIBRARY_PATH:$(brew --prefix zstd)/lib/
# elif which -s port; then
# 	port install libiconv
# 	port install zstd
# 	export LIBRARY_PATH=$LIBRARY_PATH:/opt/local/lib
# else
# 	echo "Need either brew or macports to successfully build deps"
# 	exit 1
# fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_BUILD_DIR="$PROJECT_DIR/build/$ARCH"
DEPS_DIR="$PROJECT_DIR/deps"

# KLIPSLICE_BUILD_DIR moves the slicer build tree (configure, build, packaged
# app, tests) away from build/<arch>. The universal step lipos the fixed
# build/arm64 and build/x86_64 trees, so the override is single-arch only.
if [ -n "${KLIPSLICE_BUILD_DIR:-}" ]; then
    if [ "$ARCH" = "universal" ] || [ "$BUILD_TARGET" = "universal" ]; then
        echo "ERROR: KLIPSLICE_BUILD_DIR cannot be used for universal builds" >&2
        exit 1
    fi
    case "$KLIPSLICE_BUILD_DIR" in
        /*) ;;
        *) KLIPSLICE_BUILD_DIR="$PROJECT_DIR/$KLIPSLICE_BUILD_DIR" ;;
    esac
    echo " - KLIPSLICE_BUILD_DIR: $KLIPSLICE_BUILD_DIR"
fi

# --- SDK selection -----------------------------------------------------------
# The slicer must compile against the same SDK as its dependencies: a newer SDK
# declares APIs (e.g. CGToneMapping) newer than the deployment target, and
# -Werror=unguarded-availability-new then fails the build. The deps build
# records its SDK in OrcaSlicer_dep/klipslice-sdk.txt, the directory CI keeps
# as the deps artifact; the slicer configure reads it from there.
SDK_RECORD_NAME="klipslice-sdk.txt"

# Prints the absolute, symlink-free path of an SDK. A value containing "/" or
# ending in ".sdk" is a path (a relative one resolves against the current
# directory); any other value is an SDK name such as "macosx" or "macosx26.5",
# resolved with xcrun. Returns 1 when the SDK cannot be located.
function canonical_sdk_path() {
    local value="$1"
    local path
    case "$value" in
        */*|*.sdk) path="$value" ;;
        *) path="$(xcrun --sdk "$value" --show-sdk-path 2>/dev/null)" || return 1 ;;
    esac
    if [ -z "$path" ] || [ ! -d "$path" ]; then
        return 1
    fi
    (CDPATH='' cd -- "$path" && pwd -P)
}

# Prints the value of KEY from a KEY=VALUE file (first match, empty if absent).
function read_key_value() {
    awk -v key="$1" 'index($0, key "=") == 1 { print substr($0, length(key) + 2); exit }' "$2"
}

# Prints the CMAKE_OSX_SYSROOT of a CMake cache (empty if absent).
function read_cache_sysroot() {
    awk '/^CMAKE_OSX_SYSROOT:[A-Z]*=/ { sub(/^[^=]*=/, ""); print; exit }' "$1"
}

# Writes DEPS_BUILD_DIR/OrcaSlicer_dep/klipslice-sdk.txt with the absolute path
# and version of the SDK the deps were configured with.
function write_deps_sdk_record() {
    local deps_build_dir="$1"
    local record="$deps_build_dir/OrcaSlicer_dep/$SDK_RECORD_NAME"
    local configured sdk version
    configured="$(read_cache_sysroot "$deps_build_dir/CMakeCache.txt")"
    if [ -z "$configured" ]; then
        # CMake left the sysroot empty, so the compiler used the active SDK.
        configured="$(xcrun --show-sdk-path)" || {
            echo "ERROR: xcrun cannot report the active SDK" >&2
            return 1
        }
    fi
    sdk="$(canonical_sdk_path "$configured")" || {
        echo "ERROR: cannot locate the SDK the deps were configured with: $configured" >&2
        return 1
    }
    version="$(xcrun --sdk "$sdk" --show-sdk-version 2>/dev/null)" || {
        echo "ERROR: xcrun cannot report the version of SDK $sdk" >&2
        return 1
    }
    printf 'sdk_path=%s\nsdk_version=%s\n' "$sdk" "$version" > "$record.tmp" || return 1
    mv -f "$record.tmp" "$record" || return 1
    echo "Recorded deps SDK $sdk (version $version) in $record"
}

# Prints the SDK the slicer must be configured with, in this order:
#   1. KLIPSLICE_OSX_SYSROOT (already canonical, see below);
#   2. the deps SDK record: its path, or when that path does not exist on this
#      machine, the SDK of the same version under the active developer dir;
#   3. the CMAKE_OSX_SYSROOT of the deps CMakeCache.txt (path or SDK name).
# Prints nothing, with a warning, when CMake has to fall back to its default
# SDK. Returns 1 when the deps cache names an SDK that xcrun cannot resolve.
function resolve_slicer_sysroot() {
    local deps_build_dir="$1"
    local record="$deps_build_dir/OrcaSlicer_dep/$SDK_RECORD_NAME"
    local cache="$deps_build_dir/CMakeCache.txt"
    local value="" version="" sdk=""
    if [ -n "${KLIPSLICE_OSX_SYSROOT:-}" ]; then
        printf '%s\n' "$KLIPSLICE_OSX_SYSROOT"
        return 0
    fi
    if [ -f "$record" ]; then
        value="$(read_key_value sdk_path "$record")"
        version="$(read_key_value sdk_version "$record")"
        case "$value" in
            /*) if sdk="$(canonical_sdk_path "$value")"; then
                    printf '%s\n' "$sdk"
                    return 0
                fi ;;
        esac
        case "$version" in
            ''|*[!0-9.]*) version="" ;;
        esac
        if [ -n "$version" ] && sdk="$(canonical_sdk_path "macosx$version")"; then
            echo "NOTE: the deps SDK $value does not exist here; using $sdk (same SDK version $version)" >&2
            printf '%s\n' "$sdk"
            return 0
        fi
        echo "WARNING: the deps were built with SDK ${version:-of unknown version} ($value)," >&2
        echo "         which is not installed here; CMake uses its default SDK, and" >&2
        echo "         -Werror=unguarded-availability-new can fail the build if it is newer." >&2
        echo "         Set KLIPSLICE_OSX_SYSROOT to the path of a MacOSX${version}.sdk to match the deps." >&2
        return 0
    fi
    if [ -f "$cache" ]; then
        value="$(read_cache_sysroot "$cache")"
    fi
    if [ -n "$value" ]; then
        if sdk="$(canonical_sdk_path "$value")"; then
            printf '%s\n' "$sdk"
            return 0
        fi
        case "$value" in
            */*|*.sdk)
                echo "WARNING: the deps were built with SDK $value, which no longer exists;" >&2
                echo "         CMake uses its default SDK. Set KLIPSLICE_OSX_SYSROOT to match the deps." >&2
                return 0 ;;
            *)
                echo "ERROR: $cache names the SDK '$value', which xcrun cannot resolve." >&2
                echo "       Set KLIPSLICE_OSX_SYSROOT to the SDK the deps were built with." >&2
                return 1 ;;
        esac
    fi
    echo "WARNING: no SDK recorded in $record or $cache and KLIPSLICE_OSX_SYSROOT unset;" >&2
    echo "         CMake uses its default SDK. If it is newer than the SDK the deps were built with," >&2
    echo "         -Werror=unguarded-availability-new can fail the build. Set KLIPSLICE_OSX_SYSROOT." >&2
}

# Relative override paths resolve against the directory the script is run
# from; they are made absolute here because the configure steps run in the
# deps and slicer build directories.
if [ -n "${KLIPSLICE_OSX_SYSROOT:-}" ]; then
    if ! KLIPSLICE_OSX_SYSROOT_ABS="$(canonical_sdk_path "$KLIPSLICE_OSX_SYSROOT")"; then
        echo "ERROR: KLIPSLICE_OSX_SYSROOT is neither an existing SDK directory nor an SDK name xcrun can resolve: $KLIPSLICE_OSX_SYSROOT" >&2
        exit 1
    fi
    export KLIPSLICE_OSX_SYSROOT="$KLIPSLICE_OSX_SYSROOT_ABS"
    echo " - KLIPSLICE_OSX_SYSROOT: $KLIPSLICE_OSX_SYSROOT"
fi

# For Multi-config generators like Ninja and Xcode
export BUILD_DIR_CONFIG_SUBDIR="/$BUILD_CONFIG"

function build_deps() {
    # iterate over two architectures: x86_64 and arm64
    for _ARCH in x86_64 arm64; do
        # if ARCH is universal or equal to _ARCH
        if [ "$ARCH" == "universal" ] || [ "$ARCH" == "$_ARCH" ]; then

            PROJECT_BUILD_DIR="$PROJECT_DIR/build/$_ARCH"
            DEPS_BUILD_DIR="$DEPS_DIR/build/$_ARCH"
            DEPS="$DEPS_BUILD_DIR/OrcaSlicer_dep"

            echo "Building deps..."
            (
                set -x
                mkdir -p "$DEPS"
                cd "$DEPS_BUILD_DIR"
                if [ "1." != "$BUILD_ONLY". ]; then
                    cmake "${DEPS_DIR}" \
                        -G "${DEPS_CMAKE_GENERATOR}" \
                        -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" \
                        -DCMAKE_OSX_ARCHITECTURES:STRING="${_ARCH}" \
                        -DCMAKE_OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET}" \
                        -DCMAKE_IGNORE_PREFIX_PATH="${CMAKE_IGNORE_PREFIX_PATH}" \
                        ${KLIPSLICE_OSX_SYSROOT:+"-DCMAKE_OSX_SYSROOT=$KLIPSLICE_OSX_SYSROOT"} \
                        ${CMAKE_POLICY_COMPAT}
                fi
                cmake --build . --config "$BUILD_CONFIG" --target deps
            )
            write_deps_sdk_record "$DEPS_BUILD_DIR" || exit 1
        fi
    done
}

function pack_deps() {
    echo "Packing deps..."
    (
        set -x
        cd "$DEPS_DIR"
        tar -zcvf "OrcaSlicer_dep_mac_${ARCH}_$(date +"%Y%m%d").tar.gz" "build"
    )
}

# codesign cannot seal the runtime's dotted directories (include/python3.12,
# lib/python3.12) anywhere under Contents/MacOS -- it mistakes any dotted
# directory there for a nested bundle and fails with "bundle format
# unrecognized" -- so packaged apps ship the runtime under Contents/Resources
# with a compatibility symlink that keeps every Contents/MacOS/python path and
# the @executable_path/python/lib rpath resolving unchanged.
function relocate_python_runtime() {
    local app="$1"
    local pydir="$app/Contents/MacOS/python"
    if [ -d "$pydir" ] && [ ! -L "$pydir" ]; then
        rm -rf "$app/Contents/Resources/python"
        mv "$pydir" "$app/Contents/Resources/python"
        ln -s ../Resources/python "$pydir"
    fi
}

# --- Bundled Python runtime verification --------------------------------------
# Relocation is handled at the source: deps/python3/python3.cmake stamps
# libpython with an @rpath id and src/CMakeLists.txt gives the app a matching
# rpath. This gate catches regressions that would otherwise only surface as
# launch failures on end users' machines (the absolute deps path still exists
# on the build host, so a plain run can pass while relocation is broken --
# hence the otool checks). The x86_64 leg runs under Rosetta on arm64 hosts.
function verify_python_runtime() {
    local app="$1"
    local pydir="$app/Contents/MacOS/python"
    [ -d "$pydir" ] || return 0  # app doesn't bundle Python (e.g. profile validator)
    if [ ! -L "$pydir" ]; then
        echo "ERROR: Contents/MacOS/python must be a symlink into Contents/Resources" >&2
        echo "       (see relocate_python_runtime in this script)" >&2
        exit 1
    fi
    # Version-agnostic interpreter name so a CPython version bump cannot
    # silently skip the gate; if the dir exists the interpreter must too.
    local pybin="$pydir/bin/python3"
    if [ ! -x "$pybin" ]; then
        echo "ERROR: bundled python/ present but no interpreter at $pybin" >&2
        exit 1
    fi
    echo "  Verifying bundled Python runtime in $(basename "$app")..."
    local bad
    bad=$(otool -arch all -L "$pybin" "$app/Contents/MacOS/KLIPSLICE" | grep "libpython" | grep -v "@rpath/" || true)
    if [ -n "$bad" ]; then
        echo "ERROR: a bundled binary references libpython by absolute path (relocation regression):" >&2
        echo "$bad" >&2
        exit 1
    fi
    # otool -L shows load commands only; assert the consumer rpath separately.
    # Its loss is masked on the build host by CMake's absolute build-tree rpath.
    if ! otool -arch all -l "$app/Contents/MacOS/KLIPSLICE" | grep -q "path @executable_path/python/lib "; then
        echo "ERROR: KLIPSLICE lacks the @executable_path/python/lib rpath (relocation regression)" >&2
        exit 1
    fi
    if ! "$pybin" -c "import ssl"; then
        echo "ERROR: bundled Python failed to start (libpython relocation broken," >&2
        echo "       or missing Rosetta 2 for the x86_64 leg?)" >&2
        exit 1
    fi
}

function build_slicer() {
    # iterate over two architectures: x86_64 and arm64
    for _ARCH in x86_64 arm64; do
        # if ARCH is universal or equal to _ARCH
        if [ "$ARCH" == "universal" ] || [ "$ARCH" == "$_ARCH" ]; then

            PROJECT_BUILD_DIR="${KLIPSLICE_BUILD_DIR:-$PROJECT_DIR/build/$_ARCH}"
            DEPS_BUILD_DIR="$DEPS_DIR/build/$_ARCH"
            DEPS="$DEPS_BUILD_DIR/OrcaSlicer_dep"

            # Only a build directory named after the arch lets CMakeLists.txt
            # derive DEP_BUILD_DIR, so a relocated build tree passes it.
            DEP_BUILD_DIR_ARG=()
            if [ -n "${KLIPSLICE_BUILD_DIR:-}" ]; then
                DEP_BUILD_DIR_ARG=("-DDEP_BUILD_DIR=$DEPS_BUILD_DIR")
            fi

            SYSROOT_ARG=()
            if [ "1." != "$BUILD_ONLY". ]; then
                SLICER_SYSROOT="$(resolve_slicer_sysroot "$DEPS_BUILD_DIR")" || exit 1
                if [ -n "$SLICER_SYSROOT" ]; then
                    echo "Using SDK $SLICER_SYSROOT for the slicer (same as the deps)"
                    SYSROOT_ARG=("-DCMAKE_OSX_SYSROOT=$SLICER_SYSROOT")
                fi
            fi

            echo "Building slicer for $_ARCH..."
            (
                set -x
            mkdir -p "$PROJECT_BUILD_DIR"
            cd "$PROJECT_BUILD_DIR"
            if [ "1." != "$BUILD_ONLY". ]; then
                cmake "${PROJECT_DIR}" \
                    -G "${SLICER_CMAKE_GENERATOR}" \
                    -DORCA_TOOLS=ON \
                    ${ORCA_UPDATER_SIG_KEY:+-DORCA_UPDATER_SIG_KEY="$ORCA_UPDATER_SIG_KEY"} \
                    ${BUILD_TESTS:+-DBUILD_TESTS=ON} \
                    -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" \
                    -DCMAKE_OSX_ARCHITECTURES="${_ARCH}" \
                    -DCMAKE_OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET}" \
                    -DCMAKE_IGNORE_PREFIX_PATH="${CMAKE_IGNORE_PREFIX_PATH}" \
                    ${SYSROOT_ARG[@]+"${SYSROOT_ARG[@]}"} \
                    ${DEP_BUILD_DIR_ARG[@]+"${DEP_BUILD_DIR_ARG[@]}"} \
                    ${CMAKE_POLICY_COMPAT}
            fi
            cmake --build . --config "$BUILD_CONFIG" --target "$SLICER_BUILD_TARGET"
        )

        # -T also runs the tests; ORCA_TESTS_BUILD_ONLY=1 builds them without
        # running, so CI can build here and run them in a dedicated job.
        if [ "1." == "$BUILD_TESTS". ] && [ "1." != "$ORCA_TESTS_BUILD_ONLY". ]; then
            "$PROJECT_DIR/scripts/run_unit_tests.sh" "$PROJECT_BUILD_DIR/tests" "$BUILD_CONFIG"
        fi

        echo "Verify localization with gettext..."
        (
            cd "$PROJECT_DIR"
            ./scripts/run_gettext.sh
        )

        echo "Fix macOS app package..."
        (
            cd "$PROJECT_BUILD_DIR"
            mkdir -p OrcaSlicer
            cd OrcaSlicer
            # remove previously built app
            rm -rf ./KLIPSLICE.app
            # fully copy newly built app
            cp -pR "../src$BUILD_DIR_CONFIG_SUBDIR/KLIPSLICE.app" ./KLIPSLICE.app
            # fix resources
            resources_path=$(readlink ./KLIPSLICE.app/Contents/Resources)
            rm ./KLIPSLICE.app/Contents/Resources
            cp -R "$resources_path" ./KLIPSLICE.app/Contents/Resources
            relocate_python_runtime ./KLIPSLICE.app
            # delete .DS_Store file
            find ./KLIPSLICE.app/ -name '.DS_Store' -delete

            verify_python_runtime ./KLIPSLICE.app
            
            # Copy OrcaSlicer_profile_validator.app if it exists
            if [ -f "../src$BUILD_DIR_CONFIG_SUBDIR/OrcaSlicer_profile_validator.app/Contents/MacOS/OrcaSlicer_profile_validator" ]; then
                echo "Copying OrcaSlicer_profile_validator.app..."
                rm -rf ./OrcaSlicer_profile_validator.app
                cp -pR "../src$BUILD_DIR_CONFIG_SUBDIR/OrcaSlicer_profile_validator.app" ./OrcaSlicer_profile_validator.app
                # delete .DS_Store file
                find ./OrcaSlicer_profile_validator.app/ -name '.DS_Store' -delete
                verify_python_runtime ./OrcaSlicer_profile_validator.app
            fi
        )

        # extract version
        # export ver=$(grep '^#define SoftFever_VERSION' ../src/libslic3r/libslic3r_version.h | cut -d ' ' -f3)
        # ver="_V${ver//\"}"
        # echo $PWD
        # if [ "1." != "$NIGHTLY_BUILD". ];
        # then
        #     ver=${ver}_dev
        # fi

        # zip -FSr OrcaSlicer${ver}_Mac_${_ARCH}.zip KLIPSLICE.app

    fi
    done
}

function lipo_dir() {
    local universal_dir="$1"
    local x86_64_dir="$2"

    # Find all Mach-O files in the universal (arm64-based) copy and lipo them
    while IFS= read -r -d '' f; do
        local rel="${f#"$universal_dir"/}"
        local x86="$x86_64_dir/$rel"
        if [ -f "$x86" ]; then
            echo "  lipo: $rel"
            lipo -create "$f" "$x86" -output "$f.tmp"
            mv "$f.tmp" "$f"
        else
            echo "  warning: no x86_64 counterpart for $rel, keeping arm64 only"
        fi
    done < <(find "$universal_dir" -type f -print0 | while IFS= read -r -d '' candidate; do
        if file "$candidate" | grep -q "Mach-O"; then
            printf '%s\0' "$candidate"
        fi
    done)
}

function build_universal() {
    echo "Building universal binary..."

    PROJECT_BUILD_DIR="$PROJECT_DIR/build/$ARCH"
    ARM64_APP="$PROJECT_DIR/build/arm64/OrcaSlicer/KLIPSLICE.app"
    X86_64_APP="$PROJECT_DIR/build/x86_64/OrcaSlicer/KLIPSLICE.app"

    mkdir -p "$PROJECT_BUILD_DIR/OrcaSlicer"
    UNIVERSAL_APP="$PROJECT_BUILD_DIR/OrcaSlicer/KLIPSLICE.app"
    rm -rf "$UNIVERSAL_APP"
    cp -R "$ARM64_APP" "$UNIVERSAL_APP"

    echo "Creating universal binaries for KLIPSLICE.app..."
    lipo_dir "$UNIVERSAL_APP" "$X86_64_APP"
    echo "Universal KLIPSLICE.app created at $UNIVERSAL_APP"
    verify_python_runtime "$UNIVERSAL_APP"

    # Create universal binary for profile validator if it exists
    ARM64_VALIDATOR="$PROJECT_DIR/build/arm64/OrcaSlicer/OrcaSlicer_profile_validator.app"
    X86_64_VALIDATOR="$PROJECT_DIR/build/x86_64/OrcaSlicer/OrcaSlicer_profile_validator.app"
    if [ -d "$ARM64_VALIDATOR" ] && [ -d "$X86_64_VALIDATOR" ]; then
        echo "Creating universal binaries for OrcaSlicer_profile_validator.app..."
        UNIVERSAL_VALIDATOR_APP="$PROJECT_BUILD_DIR/OrcaSlicer/OrcaSlicer_profile_validator.app"
        rm -rf "$UNIVERSAL_VALIDATOR_APP"
        cp -R "$ARM64_VALIDATOR" "$UNIVERSAL_VALIDATOR_APP"
        lipo_dir "$UNIVERSAL_VALIDATOR_APP" "$X86_64_VALIDATOR"
        echo "Universal OrcaSlicer_profile_validator.app created at $UNIVERSAL_VALIDATOR_APP"
    fi
}

case "${BUILD_TARGET}" in
    all)
        build_deps
        build_slicer
        ;;
    deps)
        build_deps
        ;;
    slicer)
        build_slicer
        ;;
    universal)
        build_universal
        ;;
    *)
        echo "Unknown target: $BUILD_TARGET. Available targets: deps, slicer, universal, all."
        exit 1
        ;;
esac

if [ "$ARCH" = "universal" ] && { [ "$BUILD_TARGET" = "all" ] || [ "$BUILD_TARGET" = "slicer" ]; }; then
    build_universal
fi

if [ "1." == "$PACK_DEPS". ]; then
    pack_deps
fi

elapsed=$SECONDS
printf "\nBuild completed in %dh %dm %ds\n" $((elapsed/3600)) $((elapsed%3600/60)) $((elapsed%60))
