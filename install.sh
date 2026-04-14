#!/bin/bash
set -euo pipefail

PLUGIN_NAME="capture-output"
OMZ_CUSTOM="${ZSH_CUSTOM:-$HOME/.oh-my-zsh/custom}"
PLUGIN_DIR="${OMZ_CUSTOM}/plugins/${PLUGIN_NAME}"

echo "═══ capture-output installer ═══"
echo ""

# 1. Check prerequisites
if ! command -v cc &>/dev/null; then
    echo "✗ C compiler not found. Install Xcode CLT: xcode-select --install"
    exit 1
fi

if [[ ! -d "${OMZ_CUSTOM}" ]]; then
    echo "✗ Oh My Zsh custom dir not found at ${OMZ_CUSTOM}"
    exit 1
fi

# 2. Copy plugin to OMZ
echo "→ Installing plugin to ${PLUGIN_DIR}"
if [[ -d "${PLUGIN_DIR}" ]]; then
    echo "  (existing installation found, updating)"
    rm -rf "${PLUGIN_DIR}"
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cp -R "${SCRIPT_DIR}" "${PLUGIN_DIR}"

# 3. Build C binaries
echo "→ Building C binaries..."
cd "${PLUGIN_DIR}"
make clean
make

echo ""
echo "→ Built:"
ls -lh bin/

# 4. Optionally install to /usr/local/bin
echo ""
read -p "Install binaries to /usr/local/bin? [y/N] " choice
if [[ "${choice}" =~ ^[Yy]$ ]]; then
    make install
    echo "  ✓ installed to /usr/local/bin"
fi

# 5. Instructions
echo ""
echo "═══ Setup ═══"
echo ""
echo "1. Add 'capture-output' to your plugins in ~/.zshrc:"
echo ""
echo "   plugins=( ... capture-output )"
echo ""
echo "2. Add this BEFORE the 'source \$ZSH/oh-my-zsh.sh' line:"
echo ""
echo '   # Auto-launch capture wrapper'
echo '   if [[ -z "$ZSH_CAPTURE_ACTIVE" ]] && command -v zsh-capture-wrapper &>/dev/null; then'
echo '       exec zsh-capture-wrapper'
echo '   fi'
echo ""
echo "3. Restart your terminal."
echo ""
echo "═══ Usage ═══"
echo ""
echo "   \$ ls -la           # any command"
echo "   \$ clc              # → clipboard (with ANSI)"
echo "   \$ clc --strip      # → clipboard (plain text)"
echo "   \$ clc --print      # → stdout"
echo "   \$ clc --info       # buffer stats"
echo ""
echo "✓ Done!"
