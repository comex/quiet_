#!/bin/sh
files="$1"
if [ -z "$files" ]; then
    files="$(git ls-files | grep '\.[ch]$' | grep -v '/\(shader1\|shader2\|elf\|lz4\|compiler-rt\)\.[ch]$')"
fi
perl -pi -e 's/(?<!#define )\bnullable(_base_ty)?\((([^\(\)]|\((?3)\))*)\)/nullable\1<\2>/g' $files
/usr/local/opt/llvm/bin/clang-format -i $files || exit 1
perl -pi -e 's/\bnullable(_base_ty)?<([^>]*)>/nullable\1(\2)/g' $files
