# Source this: `. ./env.sh`. Sets up the PS5 cross toolchain for every spike.
PS5WK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")" && pwd)"
export PS5WK_ROOT
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-$PS5WK_ROOT/toolchain/ps5-payload-sdk}"
export LLVM_CONFIG="${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}"
export PATH="$PS5_PAYLOAD_SDK/bin:$PATH"
# Console address and elfldr port; override in your shell or a local env.local.sh
export PS5_HOST="${PS5_HOST:-ps5}"
export PS5_PORT="${PS5_PORT:-9021}"
# Port the Mac listens on for spike logs (tools/logserver.sh)
export PS5WK_LOG_PORT="${PS5WK_LOG_PORT:-9999}"
# This Mac's address as the console sees it; compiled into title builds
export PS5WK_LOG_HOST="${PS5WK_LOG_HOST:-$(ipconfig getifaddr en0 2>/dev/null || echo 192.168.1.100)}"
if [ -f "$PS5WK_ROOT/env.local.sh" ]; then . "$PS5WK_ROOT/env.local.sh"; fi
