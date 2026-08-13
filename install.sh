#!/bin/sh
# Cobra Language System Package & One-Line Installer Script
# Usage: curl -fsSL https://raw.githubusercontent.com/Aaron-Savron/Cobra/main/install.sh | sh
#
# Copyright (c) 2026 The Cobra Project Authors.

set -e

COBRA_VERSION="v1.0.0"
INSTALL_DIR="${COBRA_INSTALL_DIR:-$HOME/.cobra}"
BIN_DIR="$INSTALL_DIR/bin"
LIB_DIR="$INSTALL_DIR/lib"
SYSTEM_BIN="/usr/local/bin"

echo "[install] Installing Cobra Systems Language ($COBRA_VERSION)..."

# 1. Architecture & OS Verification
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

if [ "$OS" != "linux" ]; then
    echo "Error: Direct native installation currently supports Linux x86_64. For WebAssembly / Win64 / ARM64 targets, build from source."
    exit 1
fi

if [ "$ARCH" != "x86_64" ]; then
    echo "Error: Architecture $ARCH is not supported yet. Cobra requires x86_64."
    exit 1
fi

# 2. CPU Hardware Capability Check (AVX2 Check)
if grep -q avx2 /proc/cpuinfo; then
    echo "[hardware] AVX2 256-bit SIMD instructions supported."
else
    echo "[warning] AVX2 not detected in /proc/cpuinfo."
fi

# 3. Setup Installation Directory Structure
mkdir -p "$BIN_DIR" "$LIB_DIR"

# 4. Deploy Compiler & Standard Library Assets
echo "[deploy] Deploying compiler binary..."
if [ -f "./cobra" ]; then
    cp ./cobra "$BIN_DIR/cobra"
elif [ -f "./bin/cobra" ]; then
    cp ./bin/cobra "$BIN_DIR/cobra"
else
    echo "[download] Fetching latest Cobra binary from GitHub..."
    curl -fsSL "https://raw.githubusercontent.com/Aaron-Savron/Cobra/main/dist/cobra-v1.0.0-linux-x86_64.tar.gz" -o /tmp/cobra.tar.gz 2>/dev/null || true
    if [ -f /tmp/cobra.tar.gz ]; then
        tar -xzf /tmp/cobra.tar.gz -C /tmp/
        cp /tmp/cobra-v1.0.0-linux-x86_64/bin/cobra "$BIN_DIR/cobra"
        cp /tmp/cobra-v1.0.0-linux-x86_64/lib/std.cb "$LIB_DIR/std.cb"
        [ -f /tmp/cobra-v1.0.0-linux-x86_64/lib/nn.cb ] && cp /tmp/cobra-v1.0.0-linux-x86_64/lib/nn.cb "$LIB_DIR/nn.cb"
        [ -f /tmp/cobra-v1.0.0-linux-x86_64/lib/fs.cb ] && cp /tmp/cobra-v1.0.0-linux-x86_64/lib/fs.cb "$LIB_DIR/fs.cb"
        [ -f /tmp/cobra-v1.0.0-linux-x86_64/lib/time.cb ] && cp /tmp/cobra-v1.0.0-linux-x86_64/lib/time.cb "$LIB_DIR/time.cb"
        [ -f /tmp/cobra-v1.0.0-linux-x86_64/lib/mem.cb ] && cp /tmp/cobra-v1.0.0-linux-x86_64/lib/mem.cb "$LIB_DIR/mem.cb"
        [ -f /tmp/cobra-v1.0.0-linux-x86_64/lib/cpu.cb ] && cp /tmp/cobra-v1.0.0-linux-x86_64/lib/cpu.cb "$LIB_DIR/cpu.cb"
        [ -f /tmp/cobra-v1.0.0-linux-x86_64/lib/cobra_parallel.c ] && cp /tmp/cobra-v1.0.0-linux-x86_64/lib/cobra_parallel.c "$LIB_DIR/cobra_parallel.c"
        rm -rf /tmp/cobra.tar.gz /tmp/cobra-v1.0.0-linux-x86_64
    fi
fi
chmod +x "$BIN_DIR/cobra"

echo "[deploy] Deploying standard library to $LIB_DIR/std.cb..."
if [ -f "./lib/std.cb" ]; then
    cp ./lib/std.cb "$LIB_DIR/std.cb"
fi
if [ -f "./lib/nn.cb" ]; then
    cp ./lib/nn.cb "$LIB_DIR/nn.cb"
fi
if [ -f "./lib/fs.cb" ]; then
    cp ./lib/fs.cb "$LIB_DIR/fs.cb"
fi
if [ -f "./lib/time.cb" ]; then
    cp ./lib/time.cb "$LIB_DIR/time.cb"
fi
if [ -f "./lib/mem.cb" ]; then
    cp ./lib/mem.cb "$LIB_DIR/mem.cb"
fi
if [ -f "./lib/cpu.cb" ]; then
    cp ./lib/cpu.cb "$LIB_DIR/cpu.cb"
fi
if [ -f "./runtime/cobra_parallel.c" ]; then
    cp ./runtime/cobra_parallel.c "$LIB_DIR/cobra_parallel.c"
fi

# 5. System Package Symlink (/usr/local/bin/cobra)
if [ -w "$SYSTEM_BIN" ]; then
    echo "[system] Linking executable to $SYSTEM_BIN/cobra..."
    ln -sf "$BIN_DIR/cobra" "$SYSTEM_BIN/cobra"
fi

# 6. Shell PATH Configuration
SHELL_PROFILE=""
case "$SHELL" in
    */zsh)  SHELL_PROFILE="$HOME/.zshrc" ;;
    */bash) SHELL_PROFILE="$HOME/.bashrc" ;;
    *)      SHELL_PROFILE="$HOME/.profile" ;;
esac

if ! echo "$PATH" | grep -q "$BIN_DIR"; then
    echo "[path] Updating PATH in $SHELL_PROFILE..."
    printf "\n# Cobra Language Compiler\nexport PATH=\"%s:\$PATH\"\nexport COBRA_LIB_PATH=\"%s\"\n" "$BIN_DIR" "$LIB_DIR" >> "$SHELL_PROFILE"
fi

echo "----------------------------------------------------------"
echo "[success] Cobra $COBRA_VERSION successfully installed!"
echo "System Executable: $BIN_DIR/cobra"
echo "Run 'cobra run examples/01_hello.cb' to execute code."
echo "Run 'cobra update' anytime to update from GitHub."
echo "----------------------------------------------------------"
