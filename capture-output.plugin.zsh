# capture-output.plugin.zsh
#
# Oh My Zsh plugin for transparent command output capture.
#
# This plugin works with zsh-capture-wrapper (C pty wrapper) to
# capture every command's stdout/stderr into POSIX shared memory.
# Use `clc` to copy the last command's output to the clipboard.
#
# Architecture:
#   1. zsh-capture-wrapper spawns zsh inside a pty, relays all I/O
#   2. This plugin emits invisible OSC markers in preexec/precmd
#   3. The wrapper detects markers to delimit per-command output
#   4. `clc` reads shared memory → pbcopy
#
# Installation:
#   1. Copy this dir to ~/.oh-my-zsh/custom/plugins/capture-output/
#   2. Run `make` inside the plugin dir
#   3. Add 'capture-output' to plugins=(...) in .zshrc
#   4. Add the bootstrap block below to .zshrc (BEFORE oh-my-zsh.sh)

# ─── Binary discovery ───────────────────────────────────────────

# Resolve plugin directory (works even if symlinked)
typeset -g __CAP_PLUGIN_DIR="${0:A:h}"
typeset -g __CAP_BIN_DIR="${__CAP_PLUGIN_DIR}/bin"

# Add bin dir to PATH if binaries exist there
if [[ -x "${__CAP_BIN_DIR}/clc" ]]; then
    export PATH="${__CAP_BIN_DIR}:${PATH}"
fi

# ─── Guard: only emit markers if running inside the wrapper ─────

if [[ -z "${ZSH_CAPTURE_ACTIVE}" ]]; then
    # Not inside wrapper — define clc as a hint
    clc() {
        echo "⚠  Not running inside zsh-capture-wrapper."
        echo "   Add this to your .zshrc (before sourcing oh-my-zsh.sh):"
        echo ""
        echo '   if [[ -z "$ZSH_CAPTURE_ACTIVE" ]] && command -v zsh-capture-wrapper &>/dev/null; then'
        echo '       exec zsh-capture-wrapper'
        echo '   fi'
        echo ""
    }
    return 0
fi

# ─── Marker emission hooks ──────────────────────────────────────

# OSC 7770;B;<cmd>  →  BEGIN capture with command line as transcript header.
# `print -rn` uses write(2) directly and disables backslash escape
# interpretation so the command text passes through literally.
__cap_preexec() {
    print -rn -- $'\e]7770;B;'"$1"$'\a'
}

# OSC 7770;E  →  END capture (after command finishes, before prompt)
__cap_precmd() {
    print -n -- $'\e]7770;E\a'
}

autoload -Uz add-zsh-hook
add-zsh-hook preexec __cap_preexec
add-zsh-hook precmd  __cap_precmd

# __cap_preexec must fire LAST in preexec (so other hooks' output is NOT
# captured); add-zsh-hook already appends, so nothing to do.
#
# __cap_precmd must fire FIRST in precmd (so OSC title updates from
# termsupport, powerlevel10k, etc. are NOT captured). add-zsh-hook
# appends by default, so promote ours to the front of the array.
precmd_functions=(__cap_precmd ${precmd_functions:#__cap_precmd})

# ─── Convenience aliases ────────────────────────────────────────

# clc is the C binary, but add some shell sugar on top
alias clcs='clc --strip'     # strip ANSI before copying
alias clcp='clc --print'     # print to stdout instead
alias clci='clc --info'      # buffer stats
