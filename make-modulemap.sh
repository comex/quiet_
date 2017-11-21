#!/bin/bash
set -e
(for filename in {qmod,common,loader}/*.h usbducks/usbducks.h; do
    if [ "$filename" = "loader/elf.h" ]; then
        continue
    fi
    base="$(basename "${filename//.h}")"
    echo "module $base {"
    echo "header \"$filename\""
    if [ "$filename" = "common/decls.h" ] || [ "$filename" = "common/types.h" ]; then
        echo "export *"
    fi
    if [ "$filename" = "common/misc.h" ]; then
        echo "export ssprintf"
    fi
    if [ "$filename" = "common/logging.h" ]; then
        echo "export my_stdarg"
        echo "export ssprintf"
    fi
    if [ "$filename" = "common/ssprintf.h" ]; then
        echo "export my_stdarg"
    fi
    echo "}"
done) > tmp_module.modulemap
mv tmp_module.modulemap module.modulemap

