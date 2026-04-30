#!/bin/sh
# embed_skill.sh — generate the C source that embeds
# .claude/skills/ipman/SKILL.md into the binary.
#
# Usage: embed_skill.sh OUTFILE SKILL_MD
#
# The embedded bytes are used by `ipman init` to write project-local copies of
# SKILL.md for supported agent runtimes when no matching global install exists.
# Output is deterministic — re-running with the same input produces byte-
# identical results.
#
# Why a byte array instead of a string literal: C99 only requires compilers
# to support string literals up to 4095 chars, and SKILL.md is larger. A
# char[] initialized from a { ... } list has no such limit.

set -eu

if [ $# -ne 2 ]; then
    echo "usage: $0 OUTFILE SKILL_MD" >&2
    exit 1
fi

out=$1
src=$2
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

{
    printf '/* GENERATED — do not edit. Source: %s */\n' "$src"
    printf '#include "skill_install.h"\n'
    printf '#include <stddef.h>\n\n'
    printf 'const unsigned char ipman_skill_md[] = {\n'

    # Dump file as unsigned decimal bytes, 16 per line of C source, plus a
    # trailing NUL so the array can also be treated as a C string.
    od -An -v -tu1 "$src" | awk '
        {
            for (i = 1; i <= NF; i++) {
                if (count % 16 == 0) printf "    ";
                printf "%d,", $i;
                count++;
                if (count % 16 == 0) printf "\n";
            }
        }
        END { if (count % 16 != 0) printf "\n" }
    '

    printf '    0\n'
    printf '};\n\n'
    printf 'const size_t ipman_skill_md_len = sizeof ipman_skill_md - 1;\n'
} > "$tmp"

mkdir -p "$(dirname "$out")"
mv "$tmp" "$out"
