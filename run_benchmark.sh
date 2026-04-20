#!/bin/bash

# ==========================================
# Reactor WebServer 真实场景压测脚本
# 功能：1. 自动生成 .bin 测试文件 (保留 html 不动)
#       2. 自动压测 3. 生成报告
# ==========================================

echo "=========================================="
echo "  Reactor WebServer 真实场景压测"
echo "  模式：HTTP/1.1 长连接 (Keep-Alive)"
echo "=========================================="

# --------------------------
# 配置部分
# --------------------------
BASE="http://127.0.0.1:8888"
# 自动获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WWW_DIR="${SCRIPT_DIR}/www"

# 全局变量存储结果
SUMMARY_REPORT=""

# --------------------------
# 1. 自动生成 .bin 测试文件函数 (不触碰 html)
# --------------------------
generate_bin_files() {
    echo ""
    echo "[准备] 正在生成 .bin 测试文件..."
    
    # 确保 www 目录存在
    mkdir -p "$WWW_DIR"
    cd "$WWW_DIR" || exit 1

    # 只定义需要生成的 bin 文件
    declare -A bin_files=(
        ["1mb.bin"]="1"
        ["10mb.bin"]="10"
        ["100mb.bin"]="100"
    )

    for file in "${!bin_files[@]}"; do
        size_mb=${bin_files[$file]}
        target_file="${WWW_DIR}/${file}"
        
        # 强制删除并重新创建
        echo "        重新生成 $file (${size_mb}MB)..."
        rm -f "$target_file"
        # 使用 truncate 生成稀疏文件，瞬间完成，大小精准
        truncate -s "${size_mb}M" "$target_file"
    done
    
    echo "[准备] .bin 文件就绪 (HTML 文件保持原样)。"
    cd "$SCRIPT_DIR" || exit 1
}

# --------------------------
# 2. 清空缓存函数
# --------------------------
clean_cache() {
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    sleep 2
}

# --------------------------
# 3. 压测执行函数 (并记录结果)
# --------------------------
run_benchmark() {
    local name=$1
    local file=$2
    local args=$3
    local url="${BASE}/${file}"
    
    echo ""
    echo "=========================================="
    echo "  压测目标: ${name}"
    echo "  参数: ${args}"
    echo "=========================================="

    # 执行压测并捕获输出
    output=$(wrk ${args} ${url} 2>&1)
    echo "$output"

    # 解析结果 (提取 QPS 和 带宽)
    qps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}')
    transfer=$(echo "$output" | grep "Transfer/sec" | awk '{print $2, $3}')
    
    # 如果解析失败，设为 N/A
    [ -z "$qps" ] && qps="N/A"
    [ -z "$transfer" ] && transfer="N/A"

    # 追加到报告
    SUMMARY_REPORT+="| ${name} | ${qps} | ${transfer} |\n"
}

# ==========================================
# 主流程开始
# ==========================================

# Step 1: 只生成 bin 文件，HTML 不动
generate_bin_files

# Step 2: 编译服务器
echo ""
echo "[编译] 正在重新编译服务器..."
cd "$SCRIPT_DIR" || exit 1

# 清理旧的构建目录
rm -rf build
mkdir -p build
cd build || exit 1

# 生成构建文件并编译
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 检查编译是否成功
if [ $? -ne 0 ]; then
    echo "[错误] 编译失败！"
    exit 1
fi

echo "[编译] 服务器编译成功！"
cd "$SCRIPT_DIR" || exit 1

# Step 3: 启动服务器
echo ""
echo "[启动] 正在启动服务器..."

# 杀死可能存在的旧进程
pkill -f "build/bin/server" 2>/dev/null

# 启动服务器（后台运行）
./build/bin/server > server.log 2>&1 &

# 等待服务器启动
sleep 3

# 检查服务器是否成功启动
if ! pgrep -f "build/bin/server" > /dev/null; then
    echo "[错误] 服务器启动失败！"
    echo "查看日志：cat server.log"
    exit 1
fi

echo "[启动] 服务器启动成功，正在监听端口 8888..."

# Step 4: 首次清缓存
clean_cache

# 初始化报告表头
SUMMARY_REPORT+="\n"
SUMMARY_REPORT+="==========================================\n"
SUMMARY_REPORT+="  压测总结报告\n"
SUMMARY_REPORT+="==========================================\n"
SUMMARY_REPORT+="| 文件名称 | QPS (Requests/sec) | 带宽 (Transfer/sec) |\n"
SUMMARY_REPORT+="|----------|-------------------|---------------------|\n"

# --------------------------
# 正式压测循环
# --------------------------
run_benchmark "welcome.html" "welcome.html" "-t12 -c400 -d30s"
clean_cache

run_benchmark "index.html" "index.html" "-t12 -c400 -d30s"
clean_cache

run_benchmark "1mb.bin" "1mb.bin" "-t12 -c200 -d30s"
clean_cache

run_benchmark "10mb.bin" "10mb.bin" "-t12 -c150 -d30s"
clean_cache

run_benchmark "100mb.bin" "100mb.bin" "-t12 -c100 -d60s --timeout 10s"

# ==========================================
# 打印最终报告
# ==========================================
echo -e "$SUMMARY_REPORT"
echo "=========================================="
echo "  压测全部完成！"
echo "=========================================="

# Step 5: 停止服务器
echo ""
echo "[清理] 正在停止服务器..."
pkill -f "build/bin/server" 2>/dev/null
sleep 1

if ! pgrep -f "build/bin/server" > /dev/null; then
    echo "[清理] 服务器已成功停止。"
else
    echo "[警告] 服务器停止失败，可能需要手动终止。"
fi
