#!/bin/bash
set -e

cat <<EOF >> /root/.gitconfig
[url "https://gh-proxy.test.osinfra.cn/https://github.com/"]
insteadOf = https://github.com/
EOF

# ccache 未安装时自动退化，不影响编译；缓存目录与 workflow 中的 path 保持一致
CCACHE_ARGS=()
if command -v ccache >/dev/null 2>&1; then
  export CCACHE_DIR="$HOME/.ccache"
  export CCACHE_BASEDIR="$PWD"
  export CCACHE_NOHASHDIR=1
  export CCACHE_MAXSIZE=2G
  CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  echo "ccache enabled: $CCACHE_DIR"
else
  echo "ccache not installed, build without compiler cache"
fi

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=1 \
  -DBUILD_UBLK_FRONTEND=on \
  -DENABLE_DSA=0 \
  -DENABLE_ISAL=0 \
  "${CCACHE_ARGS[@]}"

cmake --build build -j8
