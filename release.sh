#!/usr/bin/env bash
set -euo pipefail
if [ "$#" != 1 ]; then
    echo "error: no version"
    exit 1
fi
if [ -n "$(git status --porcelain)" ]; then
    echo "error: not clean"
    exit 1
fi
version="$1"
if [ -n "$(git tag -l "$version")" ]; then
    echo "error: git tag $version exists"
    exit 1
fi
if [ -e "artifacts/$version" ]; then
    echo "error: artifacts/$version exists"
    exit 1
fi
git archive --format=tar --prefix="qmod-$version/" HEAD | tar xv
rm "qmod-$version"/{.gitignore,.clang-format,release.sh}
gnutar cf source.tar --owner=0 --group=0 --mtime='1970-01-01 00:00:00' "qmod-$version"
mkdir -p "artifacts/$version"
rm -rf disttmp
mkdir -p disttmp/wiiu/apps/qmod
pushd "qmod-$version"
make -j4 obj/loader.elf EXTRA_CFLAGS="-DFIXED_BUILD_ID='\"v$version\"' -DRELEASE_MODE=1"
cp -a obj/*.dbg "../artifacts/$version/"
cp obj/loader.elf ../disttmp/wiiu/apps/qmod/
popd
rm -r "qmod-$version"
sed "s%VERSION%$version%" loader/meta.xml > disttmp/wiiu/apps/qmod/meta.xml
xz < source.tar > "artifacts/$version/source.tar.xz"
rm source.tar
#cp "artifacts/$version/source.tar.xz" disttmp/wiiu/apps/qmod/
find disttmp -exec touch -t 197001010000 -a -m {} \;
pushd disttmp
zip -r "qmod-$version.zip" wiiu
popd
mv "disttmp/qmod-$version.zip" "artifacts/$version/"
rm -rf disttmp
git add "artifacts/$version"
git commit -a -m "release.sh autocommit: $version"
git tag "$version"
