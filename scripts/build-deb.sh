#!/bin/bash

# Build script for parodus2rbus Debian packages
# Supports both amd64 and arm64 architectures

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Default values
ARCH="$(dpkg --print-architecture)"
DIST="$(lsb_release -cs)"
BUILD_DIR="$PROJECT_ROOT/build-pkg"
OUTPUT_DIR="$PROJECT_ROOT/packages"

usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Build Debian packages for parodus2rbus

OPTIONS:
    -a, --arch ARCH         Target architecture (amd64, arm64) [default: $ARCH]
    -d, --dist DIST         Target distribution [default: $DIST]
    -o, --output DIR        Output directory [default: $OUTPUT_DIR]
    -c, --clean             Clean build directory before building
    -h, --help              Show this help message

EXAMPLES:
    $0                      # Build for current architecture and distribution
    $0 -a arm64             # Cross-compile for arm64
    $0 -d jammy -a amd64    # Build for Ubuntu 22.04 (jammy) amd64
    $0 --clean              # Clean build and rebuild

DEPENDENCIES:
    This script requires the following packages to be installed:
    - debhelper-compat
    - cmake
    - build-essential
    - pkg-config
    - devscripts
    - dpkg-dev
    - fakeroot

    For cross-compilation to arm64:
    - gcc-aarch64-linux-gnu
    - g++-aarch64-linux-gnu

    RBus dependencies (may need to be built separately):
    - librbus-dev
    - librbuscore-dev
    - libcjson-dev
    - libjansson-dev
    - libcimplog-dev
    - libwrp-c-dev
    - libparodus-dev
    - uuid-dev
    - libssl-dev
EOF
}

install_deps() {
    echo "Installing build dependencies..."
    sudo apt-get update
    sudo apt-get install -y \
        debhelper-compat \
        cmake \
        build-essential \
        pkg-config \
        devscripts \
        dpkg-dev \
        fakeroot \
        libcjson-dev \
        libjansson-dev \
        uuid-dev \
        libssl-dev

    if [ "$ARCH" = "arm64" ] && [ "$(dpkg --print-architecture)" = "amd64" ]; then
        echo "Installing cross-compilation tools for arm64..."
        sudo apt-get install -y \
            gcc-aarch64-linux-gnu \
            g++-aarch64-linux-gnu
    fi
}

check_deps() {
    echo "Checking build dependencies..."
    
    missing_deps=()
    
    # Check for basic build tools
    for cmd in cmake dpkg-buildpackage fakeroot; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            missing_deps+=("$cmd")
        fi
    done
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        echo "Missing dependencies: ${missing_deps[*]}"
        echo "Run with --install-deps to install them automatically"
        return 1
    fi
    
    echo "All basic dependencies found"
}

build_package() {
    echo "Building Debian package for $ARCH ($DIST)..."
    
    # Create build directory
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    mkdir -p "$OUTPUT_DIR"
    
    # Copy source to build directory
    cp -r "$PROJECT_ROOT"/* "$BUILD_DIR/" 2>/dev/null || true
    cd "$BUILD_DIR"
    
    # Update changelog with current timestamp if building from development
    if [ ! -f .git ] || git describe --tags --exact-match HEAD >/dev/null 2>&1; then
        echo "Building from tagged release"
    else
        echo "Building development version..."
        VERSION="1.0.0-$(date +%Y%m%d)-$(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
        sed -i "1s/.*/parodus2rbus ($VERSION) $DIST; urgency=medium/" debian/changelog
    fi
    
    # Set cross-compilation environment if needed
    if [ "$ARCH" = "arm64" ] && [ "$(dpkg --print-architecture)" = "amd64" ]; then
        export CC=aarch64-linux-gnu-gcc
        export CXX=aarch64-linux-gnu-g++
        export AR=aarch64-linux-gnu-ar
        export STRIP=aarch64-linux-gnu-strip
        export PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig
        
        # Update debian/rules for cross-compilation
        if ! grep -q "DEB_HOST_ARCH" debian/rules; then
            sed -i '/^%:/i\
export DEB_HOST_ARCH := arm64\
export DEB_BUILD_GNU_TYPE := $(shell dpkg-architecture -qDEB_BUILD_GNU_TYPE)\
export DEB_HOST_GNU_TYPE := aarch64-linux-gnu\
export CC := aarch64-linux-gnu-gcc\
export CXX := aarch64-linux-gnu-g++\
' debian/rules
        fi
    fi
    
    # Build the package
    echo "Running dpkg-buildpackage..."
    dpkg-buildpackage -us -uc -b --host-arch="$ARCH"
    
    # Copy built packages to output directory
    cp ../*.deb "$OUTPUT_DIR/" 2>/dev/null || echo "No .deb files found"
    cp ../*.ddeb "$OUTPUT_DIR/" 2>/dev/null || echo "No .ddeb files found"
    
    echo "Build complete! Packages saved to: $OUTPUT_DIR"
    ls -la "$OUTPUT_DIR"
}

# Parse command line arguments
INSTALL_DEPS=false
CLEAN=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -a|--arch)
            ARCH="$2"
            shift 2
            ;;
        -d|--dist)
            DIST="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN=true
            shift
            ;;
        --install-deps)
            INSTALL_DEPS=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Validate architecture
case "$ARCH" in
    amd64|arm64)
        ;;
    *)
        echo "Unsupported architecture: $ARCH"
        echo "Supported architectures: amd64, arm64"
        exit 1
        ;;
esac

echo "Building for architecture: $ARCH"
echo "Target distribution: $DIST"
echo "Output directory: $OUTPUT_DIR"

if [ "$CLEAN" = true ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

if [ "$INSTALL_DEPS" = true ]; then
    install_deps
fi

check_deps
build_package

echo "Build script completed successfully!"