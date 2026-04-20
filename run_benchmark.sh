#!/bin/bash

# ==========================================
# Reactor WebServer 真实场景压测脚本
# 模式：HTTP/1.1 默认长连接 (Keep-Alive)
# ==========================================

echo "=========================================="
echo "  Reactor WebServer 真实场景压测"
echo "  模式：HTTP/1.1 长连接 (Keep-Alive)"
echo "=========================================="

# 全局变量存储结果
SUMMARY_REPORT=""
BASE="http://127.0.0.1:8888"

# 清空缓存函数
clean_cache() {
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    sleep 2
}

# 压测执行函数 (并记录结果)
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
# 压测开始
# ==========================================
clean_cache

# 初始化报告表头
SUMMARY_REPORT+="\n"
SUMMARY_REPORT+="==========================================\n"
SUMMARY_REPORT+="  压测总结报告\n"
SUMMARY_REPORT+="==========================================\n"
SUMMARY_REPORT+="| 文件名称 | QPS (Requests/sec) | 带宽 (Transfer/sec) |\n"
SUMMARY_REPORT+="|----------|-------------------|---------------------|\n"

# --------------------------
# 1. 极小文件 (917B)
# --------------------------
run_benchmark "welcome.html" "welcome.html" "-t12 -c400 -d30s"
clean_cache

# --------------------------
# 2. 小文件 (10.9KB)
# --------------------------
run_benchmark "index.html" "index.html" "-t12 -c400 -d30s"
clean_cache

# --------------------------
# 3. 中等文件 (1MB)
# --------------------------
run_benchmark "1mb.bin" "1mb.bin" "-t12 -c200 -d30s"
clean_cache

# --------------------------
# 4. 【新增】较大文件 (10MB)
# --------------------------
run_benchmark "10mb.bin" "10mb.bin" "-t12 -c150 -d30s"
clean_cache

# --------------------------
# 5. 大文件 (100MB)
# --------------------------
run_benchmark "100mb.bin" "100mb.bin" "-t12 -c100 -d60s --timeout 10s"

# ==========================================
# 打印最终报告
# ==========================================
echo -e "$SUMMARY_REPORT"
echo "=========================================="
echo "  压测全部完成！"
echo "=========================================="
