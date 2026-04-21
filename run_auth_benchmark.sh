#!/bin/bash

# ==========================================
# 最终极限压测版 (含自动系统调优)
# ==========================================

echo "=========================================="
echo "  WebServer 极限性能压测 (含系统调优)"
echo "=========================================="
echo ""

# --------------------------
# 配置
# --------------------------
DB_USER="root"
DB_PASS="123456"
DB_NAME="webserver_db"
SERVER_HOST="localhost"
SERVER_PORT="8888"
SUDO_PASS="123456" # sudo 密码

# 压测阶段配置
WARMUP_THREADS=4
WARMUP_CONNECTIONS=50
WARMUP_DURATION=10

STD_THREADS=12
STD_CONNECTIONS=200
STD_DURATION=30

EXTREME_THREADS=24
EXTREME_CONNECTIONS=500
EXTREME_DURATION=60

OUTPUT_DIR="benchmark_results"
REPORT_DATA=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
NC='\033[0m'

# --------------------------
# 0. 自动执行 sudo 命令的辅助函数
# --------------------------
run_sudo() {
    echo "${SUDO_PASS}" | sudo -S "$@" 2>/dev/null
}

# --------------------------
# 1. 系统调优 (新增核心功能)
# --------------------------
optimize_system() {
    echo -e "${BLUE}[系统调优]${NC}"
    
    # 1.1 调整文件描述符限制
    echo -e "${YELLOW}[1/4]${NC} 调整文件描述符限制..."
    ulimit -n 65535 2>/dev/null
    run_sudo bash -c 'ulimit -n 65535'
    echo -e "   当前用户限制: $(ulimit -n)"
    
    # 1.2 清空系统缓存
    echo -e "${YELLOW}[2/4]${NC} 清空系统缓存..."
    sync
    run_sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    sleep 1
    echo -e "   缓存已清空"
    
    # 1.3 调整网络参数 (可选优化)
    echo -e "${YELLOW}[3/4]${NC} 优化网络参数..."
    run_sudo sysctl -w net.core.somaxconn=65535 2>/dev/null
    run_sudo sysctl -w net.core.netdev_max_backlog=65535 2>/dev/null
    run_sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535 2>/dev/null
    run_sudo sysctl -w net.ipv4.tcp_tw_reuse=1 2>/dev/null
    echo -e "   网络参数已优化"
    
    # 1.4 关闭 Address Space Layout Randomization (可选，提高性能稳定性)
    echo -e "${YELLOW}[4/4]${NC} 禁用 ASLR (提高性能一致性)..."
    run_sudo sh -c 'echo 0 > /proc/sys/kernel/randomize_va_space' 2>/dev/null
    echo -e "   系统调优完成"
    
    # 检查工具
    if ! command -v wrk &> /dev/null; then echo -e "${RED}[错误]${NC} 请安装 wrk"; exit 1; fi
    if ! command -v mysql &> /dev/null; then echo -e "${RED}[错误]${NC} 请安装 mysql-client"; exit 1; fi
    
    echo -e "${GREEN}[成功]${NC} 系统调优完成"
    echo ""
}

# --------------------------
# 2. 阶段间清空缓存
# --------------------------
drop_cache_between_tests() {
    echo -e "${BLUE}[清理]${NC} 阶段间清空缓存..."
    sync
    run_sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    sleep 1
}

# --------------------------
# 3. 数据库准备
# --------------------------
prepare_db_data() {
    echo -e "${BLUE}[数据库]${NC} 准备测试数据..."
    mysql -u"${DB_USER}" -p"${DB_PASS}" "${DB_NAME}" -e "TRUNCATE TABLE users;" 2>/dev/null
    
    mysql -u"${DB_USER}" -p"${DB_PASS}" "${DB_NAME}" <<EOF
INSERT INTO users (username, password) VALUES 
('testuser', 'testpass'),
('user1', 'pass1'), ('user2', 'pass2'), ('user3', 'pass3'), ('user4', 'pass4'),
('user5', 'pass5'), ('user6', 'pass6'), ('user7', 'pass7'), ('user8', 'pass8'),
('user9', 'pass9'), ('user10', 'pass10'), ('user11', 'pass11'), ('user12', 'pass12');
EOF

    if [ $? -eq 0 ]; then echo -e "${GREEN}[成功]${NC} 测试数据插入完成"; fi
    echo ""
}

# --------------------------
# 4. 数据库清理
# --------------------------
cleanup_db() {
    echo ""
    echo -e "${BLUE}[清理]${NC} 清空 users 表数据..."
    mysql -u"${DB_USER}" -p"${DB_PASS}" "${DB_NAME}" -e "TRUNCATE TABLE users;"
    echo -e "${GREEN}[完成]${NC} 数据已清空"
}

# --------------------------
# 5. 恢复系统设置 (可选)
# --------------------------
restore_system() {
    echo ""
    echo -e "${BLUE}[恢复]${NC} 恢复部分系统设置..."
    run_sudo sh -c 'echo 2 > /proc/sys/kernel/randomize_va_space' 2>/dev/null
}

# --------------------------
# 6. 创建 Lua 脚本
# --------------------------
create_lua() {
    mkdir -p "$OUTPUT_DIR"
    
    # 1. 基础登录
    cat > "${OUTPUT_DIR}/login.lua" << 'EOF'
wrk.method = "POST"
wrk.body   = "username=testuser&password=testpass"
wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
EOF

    # 2. 注册脚本
    cat > "${OUTPUT_DIR}/register.lua" << 'EOF'
math.randomseed(os.time())
function request()
    local r = math.random(100000, 9999999)
    local body = "username=bench_"..r.."&password=123456"
    wrk.method = "POST"
    wrk.body = body
    wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
    return wrk.format()
end
EOF

    # 3. 高级登录
    cat > "${OUTPUT_DIR}/login_adv.lua" << 'EOF'
local users = {
    {u = "testuser", p = "testpass"},
    {u = "user1", p = "pass1"}, {u = "user2", p = "pass2"},
    {u = "user3", p = "pass3"}, {u = "user4", p = "pass4"},
    {u = "user5", p = "pass5"}, {u = "user6", p = "pass6"},
    {u = "user7", p = "pass7"}, {u = "user8", p = "pass8"},
    {u = "user9", p = "pass9"}, {u = "user10", p = "pass10"}
}
local index = 0
local user_count = 11
function request()
    index = index + 1
    if index > user_count then index = 1 end
    local cred = users[index]
    local body = "username=" .. cred.u .. "&password=" .. cred.p
    wrk.method = "POST"
    wrk.body = body
    wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
    return wrk.format()
end
EOF
}

# --------------------------
# 7. 执行压测
# --------------------------
run_stress_test() {
    local stage_name=$1
    local threads=$2
    local conns=$3
    local duration=$4
    local script=$5
    local api=$6
    
    echo "=========================================="
    echo -e "${PURPLE}[${stage_name}]${NC} ${threads}线程 / ${conns}并发 / ${duration}秒"
    echo "=========================================="
    
    local output=$(wrk -t${threads} -c${conns} -d${duration}s \
        -s "${OUTPUT_DIR}/${script}" \
        "http://${SERVER_HOST}:${SERVER_PORT}/${api}" 2>&1)
    
    echo "$output"
    echo ""
    
    local requests=$(echo "$output" | grep "requests in" | awk '{print $1}' | tr -d ',' || echo 0)
    local qps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}' || echo 0)
    local latency=$(echo "$output" | grep "Latency" | awk '{print $2}' || echo 0)
    
    REPORT_DATA+="| ${stage_name} | ${threads} | ${conns} | ${requests} | ${qps} | ${latency} |\n"
}

# --------------------------
# 8. 生成报告
# --------------------------
generate_extreme_report() {
    echo ""
    echo "=========================================="
    echo -e "${GREEN}[🔥 极限性能报告 🔥]${NC}"
    echo "=========================================="
    echo ""
    echo "📅 测试时间: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "💻 服务器配置: 12核CPU / 24 IO线程 / 24 MySQL连接池"
    echo "⚙️  系统调优: 已清空缓存/已调整文件描述符/已优化网络"
    echo ""
    echo "| 测试阶段 | 线程数 | 并发数 | 总请求 | QPS | 平均延迟 |"
    echo "|----------|--------|--------|--------|-----|----------|"
    echo -e "$REPORT_DATA"
    echo ""
    
    local max_qps=$(echo -e "$REPORT_DATA" | grep -v "测试阶段" | awk -F'|' '{print $6}' | sort -rn | head -n1)
    echo "🏆 极限 QPS: ${max_qps}"
    echo ""
}

# --------------------------
# 主流程
# --------------------------
main() {
    optimize_system
    prepare_db_data
    create_lua
    
    REPORT_DATA=""
    
    echo "=========================================="
    echo "  开始分阶段极限压测"
    echo "=========================================="
    echo ""
    
    # 阶段 1: 预热
    echo -e "${BLUE}[阶段 1/4]${NC} 系统预热..."
    run_stress_test "预热-登录" ${WARMUP_THREADS} ${WARMUP_CONNECTIONS} ${WARMUP_DURATION} "login.lua" "login"
    drop_cache_between_tests
    
    # 阶段 2: 标准
    echo -e "${BLUE}[阶段 2/4]${NC} 标准性能测试 (12核)..."
    run_stress_test "标准-登录" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "login_adv.lua" "login"
    drop_cache_between_tests
    
    run_stress_test "标准-注册" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "register.lua" "register"
    drop_cache_between_tests
    
    # 阶段 3: 极限
    echo -e "${BLUE}[阶段 3/4]${NC} 极限性能测试 (24线程/500并发)..."
    run_stress_test "极限-登录" ${EXTREME_THREADS} ${EXTREME_CONNECTIONS} ${EXTREME_DURATION} "login_adv.lua" "login"
    
    # 生成报告
    generate_extreme_report
    
    # 清理
    cleanup_db
    restore_system
    
    echo "=========================================="
    echo -e "${GREEN}[压测全部完成]${NC}"
    echo "=========================================="
}

trap 'echo -e "\n${RED}[中断]${NC} 正在清理..."; cleanup_db; restore_system; exit 1' INT TERM
main
