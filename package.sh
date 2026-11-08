#!/bin/sh
# Package dist/version.dll into a zip laid out for the Windrose game folder.
# Usage: sh package.sh 1.0.0-beta   -> WindroseFXCore-1.0.0-beta.zip
set -e
cd "$(dirname "$0")"
VERSION="${1:?version required}"
rm -rf pkg && mkdir -p pkg/R5/Binaries/Win64
cp dist/version.dll pkg/R5/Binaries/Win64/
sed 's/\r*$/\r/' config/WindroseFXCore.ini > pkg/R5/Binaries/Win64/WindroseFXCore.ini
sed 's/\r*$/\r/' LICENSE > pkg/WindroseFXCore-LICENSE.txt
sed -e '/^## Building from source/,$d' -e 's/\[\([^]]*\)\]([^)]*)/\1/g' README.md > pkg/WindroseFXCore-README.txt
printf '\nSource code: https://github.com/mcirish3/WindroseFXCore\n' >> pkg/WindroseFXCore-README.txt
sed -i 's/\r*$/\r/' pkg/WindroseFXCore-README.txt
rm -f "WindroseFXCore-$VERSION.zip"
(cd pkg && zip -rq "../WindroseFXCore-$VERSION.zip" .)
echo "Packaged WindroseFXCore-$VERSION.zip"
