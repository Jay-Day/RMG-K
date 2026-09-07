#!/usr/bin/env bash
#
# ./BundleDependencies.sh "./Bin/Release/RMG-K.exe" "./Bin/Release/"
#

exe="$1"
bin_dir="$2"
path="/${MSYSTEM,,}/bin"

function copyForOBJ() {
    local deps=`objdump.exe -p "$1" | grep 'DLL Name:' | sed -e "s/\t*DLL Name: //g"`
    while read -r line
	do
        findAndCopyDLL "$line"
    done <<< "$deps"
}

function findAndCopyDLL() {
    local file="$path/$1"
    
	if [ -f "$file" ] && [ ! -f "$bin_dir/$1" ]
	then
		cp "$file" "$bin_dir"
        copyForOBJ "$file"
		return 0
	fi

    return 0
}


for file in "$bin_dir"/*.exe "$bin_dir/Core"/*.dll "$bin_dir/Plugin"/*/*.dll
do
	echo "=> Copying dependencies for $file"
	copyForOBJ "$file"
done

"$path/windeployqt6" --exclude-plugins qpdf,qwebp,qgif,qtga,qtuiotouchplugin,qglib,qtiff,qmng,qwbmp \
				--no-translations "$exe"

qtpaths=""
if command -v qtpaths6 >/dev/null 2>&1
then
    qtpaths="$(command -v qtpaths6)"
elif command -v qtpaths >/dev/null 2>&1
then
    qtpaths="$(command -v qtpaths)"
elif [ -x "$path/qtpaths6" ]
then
    qtpaths="$path/qtpaths6"
fi

if [ -z "$qtpaths" ]
then
    echo "ERROR: Could not find qtpaths6 or qtpaths; cannot bundle qtbase_ja.qm." >&2
    exit 1
fi

qt_translations_dir="$("$qtpaths" --query QT_INSTALL_TRANSLATIONS)"
qtbase_translation="$qt_translations_dir/qtbase_ja.qm"
if [ ! -f "$qtbase_translation" ]
then
    echo "ERROR: qtbase_ja.qm was not found in $qt_translations_dir." >&2
    exit 1
fi

mkdir -p "$bin_dir/Data/Translations"
cp "$qtbase_translation" "$bin_dir/Data/Translations/qtbase_ja.qm"

# remove D3Dcompiler_47.dll - causes conflicts with Discord overlay injection
# RMG-K uses OpenGL exclusively; Windows system copy is used if ever needed
rm -f "$bin_dir/D3Dcompiler_47.dll"

# needed by Qt at runtime
cp "$path/libcrypto-3-x64.dll" "$bin_dir/"
cp "$path/libssl-3-x64.dll"    "$bin_dir/"
cp "$path/libjpeg-8.dll"       "$bin_dir/"

# remove *.a files
find "$bin_dir/" -name '*.a' -delete

exit 0
