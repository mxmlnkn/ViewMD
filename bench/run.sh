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
echo "Reference (same machine, earlier session, GPU-less sandbox):"
echo "  GTK3 probe ~25.3 MB RSS / 13.5 MB PSS;   GTK4 probe ~99-147 MB RSS"
echo "  full ViewMD (C++/GTK3) ~34.8 MB RSS / 20.4 MB PSS"
echo
echo "Optional: measure the full app the same way:"
echo "  ./viewmd some-doc.md & sleep 3; grep -E 'VmRSS|^Pss:' /proc/\$!/status /proc/\$!/smaps_rollup; kill \$!"
