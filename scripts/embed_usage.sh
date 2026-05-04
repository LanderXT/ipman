#!/bin/sh
# embed_usage.sh — generate the C source that embeds
# src/usage_text.txt into the binary.
#
# Usage: embed_usage.sh OUTFILE USAGE_TXT
#
# The embedded bytes are used by `print_usage` in src/main.c to write the
# post-version portion of the --usage help screen. Keeping the text in a
# separate file avoids C99's 4095-char string-literal limit.
# Output is deterministic — re-running with the same input produces byte-
# identical results.
#
# Why a byte array instead of a string literal: C99 only requires compilers
# to support string literals up to 4095 chars, and the usage text exceeds
# that. A char[] initialized from a { ... } list has no such limit.

set -eu

if [ $# -ne 2 ]; then
    echo "usage: $0 OUTFILE USAGE_TXT" >&2
    exit 1
fi

out=$1
src=$2
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

{
    printf '/* GENERATED — do not edit. Source: %s */\n' "$src"
    printf '#include <stddef.h>\n\n'
    printf 'const unsigned char ipman_usage_text[] = {\n'

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
    printf 'const size_t ipman_usage_text_len = sizeof ipman_usage_text - 1;\n'
} > "$tmp"

mkdir -p "$(dirname "$out")"
mv "$tmp" "$out"
