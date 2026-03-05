#!/usr/bin/env python3
"""
Patches kernel/scripts/build-xnu.sh with three fixes.
Run from inside lunaos-arm64/:  python3 fix-build-xnu.py
"""
import re, sys, shutil, os, stat
from pathlib import Path

TARGET = Path("kernel/scripts/build-xnu.sh")

if not TARGET.exists():
    print(f"ERROR: {TARGET} not found. Run this from inside lunaos-arm64/")
    sys.exit(1)

src = TARGET.read_text()
original = src

# ── Fix 1: Strip ^ from tag names ─────────────────────────────────────────
pat1 = re.compile(r"(sort\s+-V\s*\|\s*tail\s+-1)")
if pat1.search(src) and "sed 's/\\^$//' |" not in src:
    src = pat1.sub(r"sed 's/\\^$//' | \1", src)
    f1 = "applied"
elif "sed 's/\\^$//' |" in src:
    f1 = "already patched"
else:
    f1 = "FAILED"

# ── Fix 2: Skip dtrace build entirely, use no-op stubs ────────────────────
# dtrace requires Apple's internal 'macosx.internal' SDK which is not
# publicly available. ctfconvert/ctfmerge only add debug metadata —
# the kernel boots fine in QEMU/UTM without them.
#
# Strategy: replace the entire dtrace xcodebuild section with a check
# that looks for ctfconvert in the KDK first, then falls back to stubs.

stub_block = '''
# ── ctfconvert/ctfmerge: use KDK tools or no-op stubs ─────────────────────
# dtrace requires Apple's internal SDK and cannot be built from source.
# ctfconvert/ctfmerge only add CTF debug metadata — not needed for QEMU/UTM.
log_step "Setting up ctf tools"

CTF_STUB_DIR="${BUILD_DIR}/ctf-stubs"
mkdir -p "${CTF_STUB_DIR}"

# Check KDK first
KDK_CTF=""
for kdk in /Library/Developer/KDKs/*.kdk; do
    if [ -x "${kdk}/usr/bin/ctfconvert" ]; then
        KDK_CTF="${kdk}/usr/bin"
        break
    fi
done

if [ -n "${KDK_CTF}" ]; then
    log_info "ctfconvert: using KDK tools from ${KDK_CTF}"
    export PATH="${KDK_CTF}:${PATH}"
else
    log_info "ctfconvert: creating no-op stubs (CTF debug metadata skipped)"
    printf '#!/bin/sh\\nexit 0\\n' > "${CTF_STUB_DIR}/ctfconvert"
    printf '#!/bin/sh\\nexit 0\\n' > "${CTF_STUB_DIR}/ctfmerge"
    chmod +x "${CTF_STUB_DIR}/ctfconvert" "${CTF_STUB_DIR}/ctfmerge"
    export PATH="${CTF_STUB_DIR}:${PATH}"
fi
'''

# Find and replace the dtrace build block
# It starts with a line containing "Building dtrace" and ends before the
# next log_step / section marker
dtrace_pattern = re.compile(
    r'(log_step|log_info|#[^\n]*)\s*["\'].*[Bb]uilding dtrace.*["\'].*?\n'  # opening line
    r'.*?'                                                                     # content
    r'(?=\n\s*(?:log_step|log_info|#\s*──|\[\[|\bif\b|\bcd\b)\s)',          # stop before next section
    re.DOTALL
)

# Simpler: look for the dtrace xcodebuild call specifically and replace the whole block
# The block starts with a comment/log about dtrace and ends with ctfconvert check
dtrace_block_pattern = re.compile(
    r'(?:log_step|log_info|echo)[^\n]*[Bb]uilding dtrace[^\n]*\n.*?'
    r'(?:ctfconvert[^\n]*\n)',
    re.DOTALL
)

if dtrace_block_pattern.search(src):
    src = dtrace_block_pattern.sub(stub_block.lstrip('\n') + '\n', src, count=1)
    f2 = "applied (dtrace replaced with no-op stubs)"
elif "CTF_STUB_DIR" in src:
    f2 = "already patched"
else:
    # Fallback: just add PATH with stubs before xcodebuild calls
    # by replacing any xcodebuild line in a dtrace context
    xcode_sub = [0]
    def xsub(m):
        line = m.group(0)
        if 'CODE_SIGNING_ALLOWED' not in line and 'dtrace' in src[max(0,m.start()-500):m.start()].lower():
            xcode_sub[0] += 1
            return line.rstrip() + ' CODE_SIGN_IDENTITY="" CODE_SIGNING_ALLOWED=NO SDKROOT=macosx'
        return line
    new_src = re.sub(r'^[^\n]*xcodebuild[^\n]*$', xsub, src, flags=re.MULTILINE)
    if xcode_sub[0] > 0:
        src = new_src
        f2 = f"fallback applied ({xcode_sub[0]} xcodebuild lines patched with SDKROOT=macosx)"
    else:
        f2 = "FAILED - could not find dtrace build block"

# ── Fix 3: Stop git prompting for credentials ──────────────────────────────
before = src
src = src.replace("git ls-remote --tags", "GIT_TERMINAL_PROMPT=0 git ls-remote --tags")
src = src.replace("git clone ", "GIT_TERMINAL_PROMPT=0 git clone ")
if src != before:
    f3 = "applied"
elif "GIT_TERMINAL_PROMPT=0" in src:
    f3 = "already patched"
else:
    f3 = "FAILED"

# ── Save ───────────────────────────────────────────────────────────────────
if src == original:
    print("\nNo changes needed — script is already fully patched.")
else:
    shutil.copy(TARGET, TARGET.with_suffix(".sh.bak"))
    TARGET.write_text(src)
    print(f"Patched: {TARGET}")
    print(f"Backup:  {TARGET.with_suffix('.sh.bak')}")

print(f"\n  Fix 1 - strip ^ from tags:                {f1}")
print(f"  Fix 2 - skip dtrace / use ctf stubs:      {f2}")
print(f"  Fix 3 - no git credential prompts:        {f3}")

all_ok = all("FAILED" not in x for x in [f1, f2, f3])
if all_ok:
    print('\nAll fixes applied. Now run:\n')
    print('  export PATH="/opt/homebrew/opt/llvm/bin:$PATH"')
    print('  make kernel\n')
else:
    print('\nSome fixes failed. Share this output.')
    sys.exit(1)
