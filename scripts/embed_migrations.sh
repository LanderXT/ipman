#!/bin/sh
# embed_migrations.sh — generate the C source that registers all migrations.
#
# Usage: embed_migrations.sh OUTFILE FILE [FILE...]
#
# For each migrations/NNNN_name.sql:
#   - emits the file as a NUL-terminated unsigned char[] (byte array, not a
#     string literal: C99 only mandates 4095-char string literals, and the
#     consolidated v1 schema exceeds that).
#   - records the SHA-256 hex of the file as the migration's checksum.
# Then emits the ipman_migrations[] array sorted by the numeric prefix in
# the filename. Output is deterministic — re-running with the same inputs
# produces byte-identical results.

set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 OUTFILE MIGRATION_SQL [MIGRATION_SQL...]" >&2
    exit 1
fi

out=$1
shift
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

# Sort inputs by leading numeric prefix; independent of shell glob / locale.
sorted=$(printf '%s\n' "$@" | sort)

{
    printf '/* GENERATED — do not edit. Source: migrations/NNNN_*.sql */\n'
    printf '#include "migrations.h"\n'
    printf '#include <stddef.h>\n\n'
} > "$tmp"

# Emit each migration as a byte array with a trailing NUL so the array can
# also be treated as a C string via (const char *).
for f in $sorted; do
    base=$(basename "$f" .sql)
    ident=$(printf '%s' "$base" | tr -c 'A-Za-z0-9' _)

    printf 'static const unsigned char sql_%s[] = {\n' "$ident" >> "$tmp"
    od -An -v -tu1 "$f" | awk '
        {
            for (i = 1; i <= NF; i++) {
                if (count % 16 == 0) printf "    ";
                printf "%d,", $i;
                count++;
                if (count % 16 == 0) printf "\n";
            }
        }
        END { if (count % 16 != 0) printf "\n" }
    ' >> "$tmp"
    printf '    0\n};\n\n' >> "$tmp"
done

# Emit the registry.
printf 'const struct ipman_migration ipman_migrations[] = {\n' >> "$tmp"

count=0
for f in $sorted; do
    base=$(basename "$f" .sql)
    ident=$(printf '%s' "$base" | tr -c 'A-Za-z0-9' _)
    version=$(printf '%s' "$base" | sed -n 's/^0*\([0-9][0-9]*\).*/\1/p')
    if [ -z "$version" ]; then
        echo "embed_migrations: cannot parse version from '$base'" >&2
        exit 1
    fi

    # SHA-256 hex of the raw file bytes. `sha256sum` output is "<hex>  <path>".
    sum=$(sha256sum "$f" | cut -d' ' -f1)
    if [ -z "$sum" ]; then
        echo "embed_migrations: sha256sum failed for $f" >&2
        exit 1
    fi

    printf '    { %s, "%s", (const char *)sql_%s, "%s" },\n' \
           "$version" "$base" "$ident" "$sum" >> "$tmp"
    count=$((count + 1))
done

{
    printf '};\n\n'
    printf 'const size_t ipman_migrations_count = %d;\n' "$count"
} >> "$tmp"

mkdir -p "$(dirname "$out")"
mv "$tmp" "$out"
