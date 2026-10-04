#!/bin/bash
# Cross-build the DS-side ReShade add-on on Linux: clang-cl + lld-link against MSVC CRT / Windows SDK from xwin.
#   ./build.sh    ->  build/dsmc.addon64
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
X=${XWIN:-$HOME/.xwin}
TP="$HERE/../third_party"
mkdir -p "$HERE/build"
# clangd's flags for this machine (not in git: paths are local)
cat > "$HERE/compile_flags.txt" <<FLAGS
--driver-mode=cl
--target=x86_64-pc-windows-msvc
/std:c++20
/EHsc
/DNOMINMAX
/TP
-imsvc$X/crt/include
-imsvc$X/sdk/include/ucrt
-imsvc$X/sdk/include/um
-imsvc$X/sdk/include/shared
-I$(cd "$TP" && pwd)/reshade-include
-I$(cd "$TP" && pwd)/imgui
FLAGS
cd "$HERE/build"
clang-cl --target=x86_64-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc /MT /LD /W3 /DNOMINMAX -fuse-ld=lld-link \
	-Wno-unused-command-line-argument \
	/imsvc "$X/crt/include" /imsvc "$X/sdk/include/ucrt" /imsvc "$X/sdk/include/um" /imsvc "$X/sdk/include/shared" \
	/I "$TP/reshade-include" /I "$TP/imgui" \
	"$HERE"/src/*.cpp /Fe"dsmc.addon64" \
	/link /libpath:"$X/crt/lib/x86_64" /libpath:"$X/sdk/lib/um/x86_64" /libpath:"$X/sdk/lib/ucrt/x86_64"
rm -f ./*.obj ./*.lib ./*.exp
ls -la dsmc.addon64
