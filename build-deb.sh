#!/usr/bin/env bash

set -Eeuo pipefail

REPO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SERVICE_DIR="$REPO_DIR/RoboticsService"
PACK_DIR="$SERVICE_DIR/Package/debPack"
CONTROL_FILE="$PACK_DIR/control"

for command_name in cmake dpkg-deb; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "Error: required command '$command_name' was not found." >&2
        exit 1
    fi
done

PACKAGE_VERSION=$(awk '$1 == "Version:" { print $2; exit }' "$CONTROL_FILE")
if [[ -z "$PACKAGE_VERSION" ]]; then
    echo "Error: no Version field was found in $CONTROL_FILE." >&2
    exit 1
fi

ARCH=$(dpkg --print-architecture)
if [[ "$ARCH" != "amd64" ]]; then
    echo "Error: this script packages the x86_64 build, but the current architecture is '$ARCH'." >&2
    exit 1
fi

DISTRO_LABEL=${DISTRO_LABEL:-}
if [[ -z "$DISTRO_LABEL" && -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO_LABEL="${ID:-linux}_${VERSION_ID:-unknown}"
fi
DISTRO_LABEL=${DISTRO_LABEL:-linux}
DISTRO_LABEL=${DISTRO_LABEL//[^a-zA-Z0-9._-]/_}

echo "Building XRoboToolkit PC Service $PACKAGE_VERSION..."
bash "$SERVICE_DIR/qt-gcc.sh" "${PACKAGE_VERSION##*.}"

echo "Creating Debian package..."
bash "$PACK_DIR/setup.sh"

SOURCE_DEB="$SERVICE_DIR/Package/output/XRoboToolkit-PC-Service_${PACKAGE_VERSION}_${ARCH}.deb"
if [[ ! -f "$SOURCE_DEB" ]]; then
    echo "Error: the package was not generated at $SOURCE_DEB." >&2
    exit 1
fi

DOWNLOAD_DIR="$REPO_DIR/releases/download/v${PACKAGE_VERSION}"
PACKAGE_NAME="XRoboToolkit_PC_Service_${PACKAGE_VERSION}_${DISTRO_LABEL}_${ARCH}.deb"
mkdir -p "$DOWNLOAD_DIR"
install -m 0644 "$SOURCE_DEB" "$DOWNLOAD_DIR/$PACKAGE_NAME"

echo
echo "Package ready to commit:"
echo "  releases/download/v${PACKAGE_VERSION}/${PACKAGE_NAME}"
echo
echo "After committing and pushing to the fork, download it with:"
echo "  wget https://raw.githubusercontent.com/leoperezz/XRoboToolkit-PC-Service/main/releases/download/v${PACKAGE_VERSION}/${PACKAGE_NAME}"
