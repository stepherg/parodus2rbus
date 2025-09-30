# Debian Package Building for parodus2rbus

This repository is configured to build Debian packages for both `amd64` and `arm64` architectures using GitHub Actions.

## Quick Start

### Using GitHub Actions (Recommended)

1. **Automatic builds**: Push to `main` or `develop` branches to trigger builds
2. **Release builds**: Create a git tag starting with `v` (e.g., `v1.0.0`) to build and create a GitHub release
3. **Manual builds**: Use the "Actions" tab in GitHub and run "Build Debian Packages" manually

### Local Building

Use the provided build script:

```bash
# Build for current architecture
./scripts/build-deb.sh

# Build for arm64
./scripts/build-deb.sh --arch arm64

# Build for specific distribution
./scripts/build-deb.sh --arch amd64 --dist jammy

# Clean build
./scripts/build-deb.sh --clean

# Install dependencies and build
./scripts/build-deb.sh --install-deps
```

## Package Installation

### From GitHub Releases

1. Go to the [Releases page](https://github.com/stepherg/parodus2rbus/releases)
2. Download the appropriate `.deb` file for your architecture
3. Install:

```bash
# For amd64 systems
sudo dpkg -i parodus2rbus_*_amd64.deb

# For arm64 systems
sudo dpkg -i parodus2rbus_*_arm64.deb

# Install missing dependencies if needed
sudo apt-get install -f
```

### Manual Build and Install

```bash
# Clone the repository
git clone https://github.com/stepherg/parodus2rbus.git
cd parodus2rbus

# Install build dependencies
sudo apt-get update
sudo apt-get install -y \
    debhelper-compat \
    cmake \
    build-essential \
    pkg-config \
    devscripts \
    dpkg-dev \
    fakeroot

# Build the package
./scripts/build-deb.sh

# Install the built package
sudo dpkg -i packages/*.deb
```

## Dependencies

The parodus2rbus package depends on several RBus-related libraries:

- `librbus-dev`
- `librbuscore-dev`
- `libcjson-dev`
- `libjansson-dev`
- `libcimplog-dev`
- `libwrp-c-dev`
- `libparodus-dev`
- `uuid-dev`
- `libssl-dev`

**Note**: Some RBus dependencies may not be available in standard Ubuntu/Debian repositories and may need to be built separately or obtained from RDK-specific package repositories.

## Architecture Support

The build system supports the following architectures:

- **amd64** (x86_64) - Intel/AMD 64-bit
- **arm64** (aarch64) - ARM 64-bit

## Distribution Support

Packages are built and tested on:

- Ubuntu 20.04 (Focal)
- Ubuntu 22.04 (Jammy)
- Debian 11 (Bullseye)
- Debian 12 (Bookworm)

## GitHub Actions Configuration

The repository includes two workflows:

### 1. Build Debian Packages (`.github/workflows/build-debian.yml`)

- **Triggers**: Push to main/develop, pull requests, tags, manual dispatch
- **Matrix builds**: Multiple architectures and distributions
- **Docker-based**: Uses Docker with QEMU for cross-compilation
- **Artifacts**: Uploads built packages as artifacts
- **Releases**: Automatically creates GitHub releases for tags

### 2. Test Build (`.github/workflows/test-build.yml`)

- **Triggers**: Push and pull requests
- **Purpose**: Quick validation of debian packaging files
- **Lightweight**: Tests package structure without full build

## Customization

### Adding New Dependencies

Edit `debian/control` to add new build or runtime dependencies:

```debian
Build-Depends:
 debhelper-compat (= 13),
 cmake (>= 3.10),
 your-new-build-dep
```

### Build Options

Modify `debian/rules` to change CMake build options:

```make
override_dh_auto_configure:
    dh_auto_configure -- \
        -DCMAKE_BUILD_TYPE=Release \
        -DYOUR_OPTION=ON
```

### Version Management

- **Development builds**: Automatically get timestamped versions
- **Release builds**: Use git tag version (e.g., tag `v1.2.3` → package version `1.2.3`)

## Troubleshooting

### Missing RBus Dependencies

If RBus libraries are not available in your distribution:

1. Build RBus from source and install to system
2. Use a custom package repository
3. Modify `debian/control` to use alternative package names

### Cross-compilation Issues

For arm64 builds on amd64 systems:

```bash
# Install cross-compilation tools
sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Set environment variables
export CC=aarch64-linux-gnu-gcc
export CXX=aarch64-linux-gnu-g++
```

### Build Failures

1. Check build logs in GitHub Actions
2. Verify all dependencies are available
3. Test local build with `./scripts/build-deb.sh`
4. Check `lintian` output for packaging issues

## Contributing

When adding new features:

1. Update `debian/changelog` with new version
2. Add any new dependencies to `debian/control`
3. Test both amd64 and arm64 builds
4. Update this documentation if needed

## License

This project is licensed under the Apache License 2.0. See the `LICENSE` file for details.
