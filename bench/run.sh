#!/usr/bin/env bash
# Run the GTK3/GTK4 baseline probes and print their resident memory.
# The probes self-report VmRSS/PSS at ~2.5s and exit at ~6s.
#
# Interpretation:
#   - Values are the *toolkit baseline* for a viewer-shaped window
#     (header bar + 2 tabs + scrolled text view with ~1 KB of text).
#   - A GPU-less machine inflates the GTK4 number via llvmpipe software GL
#     (libLLVM/libgallium, ~50 MB); on a machine with a working GPU driver
#     the GTK4 baseline should land near the GTK3 one.
set -u
cd "$(dirname "$0")"

echo "== GTK3 probe =="
./gtk3_probe 2>/dev/null | grep '^PROBE'

echo "== GTK4 probe =="
./gtk4_probe 2>/dev/null | grep '^PROBE'

echo
echo "Reference (real host, AMD Radeon 780M / radeonsi; Ubuntu 24.04):"
echo "  GTK3 probe ~35.5 MB RSS /  8.6 MB PSS"
echo "  GTK4 probe ~212.6 MB RSS / 165.1 MB PSS   <- ~6x RSS / ~19x PSS"
echo "  => GTK4 is far outside the 50% memory envelope; stay on GTK3"
echo
echo "Optional: measure the full app the same way:"
echo "  ./viewmd some-doc.md & sleep 3; grep -E 'VmRSS|^Pss:' /proc/\$!/status /proc/\$!/smaps_rollup; kill \$!"
