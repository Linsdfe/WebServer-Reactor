#!/bin/bash

# ==========================================
# WebServer-Reactor 统一压测脚本
# 支持: 静态资源压测 / 认证压测 / 主从复制压测 / 容灾测试
# ==========================================

# --------------------------
# 全局配置
# --------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WWW_DIR="${SCRIPT_DIR}/www"
OUTPUT_DIR="benchmark_results"
SERVER_PID=""
MONITOR_PID=""

# 数据库配置
DB_USER="root"
DB_PASS="123456"
DB_NAME="webserver_db"
SERVER_HOST="localhost"
SERVER_PORT="8888"
SUDO_PASS="123456"

# 从库配置
MYSQL_SLAVES="127.0.0.1:3307,127.0.0.1:3308"
REDIS_SLAVES="127.0.0.1:6380,127.0.0.1:6381"

# 压测配置
WARMUP_THREADS=4
WARMUP_CONNECTIONS=50
WARMUP_DURATION=10
STD_THREADS=12
STD_CONNECTIONS=200
STD_DURATION=30
EXTREME_THREADS=24
EXTREME_CONNECTIONS=500
EXTREME_DURATION=60

# ==================== 辅助函数 ====================

run_sudo() {
    echo "${SUDO_PASS}" | sudo -S "$@" 2>/dev/null
}

print_header() {
    echo -e "${CYAN}==========================================${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}==========================================${NC}"
}

print_menu() {
    clear
    echo -e "${CYAN}"
    echo "╔══════════════════════════════════════════╗"
    echo "║   WebServer-Reactor 统一压测脚本         ║"
    echo "║   Unified Benchmark Suite                ║"
    echo "╠══════════════════════════════════════════╣"
    echo "║  1. 静态资源压测 (Static Resource)      ║"
    echo "║  2. 认证功能压测 (Authentication)       ║"
    echo "║  3. 主从复制压测 (Replication)          ║"
    echo "║  4. 容灾测试 (Failover)                 ║"
    echo "║  5. 完整测试套件 (Full Suite)           ║"
    echo "║  0. 退出 (Exit)                         ║"
    echo "╚══════════════════════════════════════════╝"
    echo -e "${NC}"
    echo -e "${YELLOW}请选择测试类型 [0-5]: ${NC}"
}

pause() {
    echo ""
    echo -e "${YELLOW}按 Enter 键继续...${NC}"
    read -r
}

# ==================== 系统调优 ====================

optimize_system() {
    echo -e "${BLUE}[系统调优]${NC}"

    echo -e "${YELLOW}[1/4]${NC} 调整文件描述符限制..."
    ulimit -n 65535 2>/dev/null
    run_sudo bash -c 'ulimit -n 65535'
    echo -e "   当前限制: $(ulimit -n)"

    echo -e "${YELLOW}[2/4]${NC} 清空系统缓存..."
    sync
    run_sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    sleep 1

    echo -e "${YELLOW}[3/4]${NC} 优化网络参数..."
    run_sudo sysctl -w net.core.somaxconn=65535 2>/dev/null
    run_sudo sysctl -w net.core.netdev_max_backlog=65535 2>/dev/null
    run_sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535 2>/dev/null
    run_sudo sysctl -w net.ipv4.tcp_tw_reuse=1 2>/dev/null

    echo -e "${YELLOW}[4/4]${NC} 禁用 ASLR..."
    run_sudo sh -c 'echo 0 > /proc/sys/kernel/randomize_va_space' 2>/dev/null

    if ! command -v wrk &> /dev/null; then
        echo -e "${RED}[错误]${NC} 请安装 wrk: apt install wrk"
        return 1
    fi

    echo -e "${GREEN}[完成]${NC} 系统调优完成"
    return 0
}

drop_cache() {
    sync
    run_sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    sleep 1
}

restore_system() {
    run_sudo sh -c 'echo 2 > /proc/sys/kernel/randomize_va_space' 2>/dev/null
}

# ==================== 服务器管理 ====================

start_server() {
    local mode=$1
    local server_args

    case "$mode" in
        "standalone")
            server_args="${SERVER_PORT} 8 localhost ${DB_USER} ${DB_PASS} ${DB_NAME}"
            ;;
        "mysql_replication")
            server_args="${SERVER_PORT} 8 localhost ${DB_USER} ${DB_PASS} ${DB_NAME} ${MYSQL_SLAVES}"
            ;;
        "full_replication")
            server_args="${SERVER_PORT} 8 localhost ${DB_USER} ${DB_PASS} ${DB_NAME} ${MYSQL_SLAVES} ${REDIS_SLAVES}"
            ;;
        *)
            server_args="${SERVER_PORT} 8 localhost ${DB_USER} ${DB_PASS} ${DB_NAME}"
            ;;
    esac

    echo -e "${BLUE}[启动]${NC} 启动服务器 ($mode 模式)"
    echo -e "  命令: ./build/bin/server $server_args"

    if [ ! -d "./build/bin" ]; then
        echo -e "${RED}[错误]${NC} 请先编译项目"
        return 1
    fi

    pkill -f "build/bin/server" 2>/dev/null
    sleep 1

    ./build/bin/server $server_args > server.log 2>&1 &
    SERVER_PID=$!

    sleep 3

    if ps -p $SERVER_PID > /dev/null; then
        echo -e "${GREEN}[成功]${NC} 服务器已启动 (PID: $SERVER_PID)"
        return 0
    else
        echo -e "${RED}[错误]${NC} 服务器启动失败"
        cat server.log
        return 1
    fi
}

stop_server() {
    if [ -n "$SERVER_PID" ] && ps -p $SERVER_PID > /dev/null 2>&1; then
        echo -e "${BLUE}[停止]${NC} 停止服务器 (PID: $SERVER_PID)"
        kill $SERVER_PID 2>/dev/null
        wait $SERVER_PID 2>/dev/null
    fi
    pkill -f "build/bin/server" 2>/dev/null
    SERVER_PID=""
}

cleanup_all() {
    stop_server
    kill $MONITOR_PID 2>/dev/null
    restore_system
}

# ==================== 数据库准备 ====================

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

    if [ $? -eq 0 ]; then echo -e "${GREEN}[成功]${NC} 测试数据已插入"; fi

    wait_for_slave_sync

    redis-cli flushall 2>/dev/null || echo -e "${YELLOW}[警告]${NC} Redis 可能未启动"
}

wait_for_slave_sync() {
    echo -e "${BLUE}[同步]${NC} 等待从库同步..."

    IFS="," read -ra SLAVE_LIST <<< "$MYSQL_SLAVES"
    for slave in "${SLAVE_LIST[@]}"; do
        host=$(echo $slave | cut -d':' -f1)
        port=$(echo $slave | cut -d':' -f2)
        echo -n "  $host:$port: "

        local waited=0
        while [ $waited -lt 10 ]; do
            local count=$(mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" "${DB_NAME}" \
                -e "SELECT COUNT(*) FROM users;" 2>/dev/null | tail -n1)
            if [ "$count" = "13" ]; then
                echo -e "${GREEN}已同步${NC}"
                break
            fi
            sleep 1
            waited=$((waited + 1))
        done

        if [ $waited -ge 10 ]; then
            echo -e "${YELLOW}超时${NC}"
        fi
    done
}

check_slave_status() {
    echo -e "${BLUE}[状态检查]${NC} 检查从库状态..."

    echo -e "${YELLOW}MySQL 从库:${NC}"
    IFS="," read -ra SLAVE_LIST <<< "$MYSQL_SLAVES"
    for slave in "${SLAVE_LIST[@]}"; do
        host=$(echo $slave | cut -d':' -f1)
        port=$(echo $slave | cut -d':' -f2)
        echo -n "  $host:$port: "
        local status=$(mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "SHOW SLAVE STATUS\G" 2>/dev/null)
        local io=$(echo "$status" | grep "Slave_IO_Running:" | awk '{print $2}')
        local sql=$(echo "$status" | grep "Slave_SQL_Running:" | awk '{print $2}')
        local lag=$(echo "$status" | grep "Seconds_Behind_Master:" | awk '{print $2}')
        if [ "$io" = "Yes" ] && [ "$sql" = "Yes" ]; then
            echo -e "${GREEN}正常${NC} (IO:$io SQL:$sql Lag:${lag}s)"
        else
            echo -e "${RED}异常${NC} (IO:$io SQL:$sql Lag:${lag}s)"
        fi
    done

    echo -e "${YELLOW}Redis 从库:${NC}"
    IFS="," read -ra SLAVE_LIST <<< "$REDIS_SLAVES"
    for slave in "${SLAVE_LIST[@]}"; do
        host=$(echo $slave | cut -d':' -f1)
        port=$(echo $slave | cut -d':' -f2)
        echo -n "  $host:$port: "
        local info=$(redis-cli -h "$host" -p "$port" info replication 2>/dev/null)
        local role=$(echo "$info" | grep "^role:" | cut -d: -f2)
        local link=$(echo "$info" | grep "^master_link_status:" | cut -d: -f2)
        if [ "$role" = "slave" ]; then
            if [ "$link" = "up" ]; then
                echo -e "${GREEN}正常${NC} (role:$role master:$link)"
            else
                echo -e "${RED}异常${NC} (role:$role master:$link)"
            fi
        else
            echo -e "${YELLOW}主库${NC} (role:$role)"
        fi
    done
}

# ==================== Lua 脚本 ====================

create_lua_scripts() {
    mkdir -p "$OUTPUT_DIR"

    cat > "${OUTPUT_DIR}/login.lua" << 'EOF'
wrk.method = "POST"
wrk.body   = "username=testuser&password=testpass"
wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
EOF

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
function request()
    index = index + 1
    if index > 11 then index = 1 end
    local cred = users[index]
    wrk.method = "POST"
    wrk.body = "username=" .. cred.u .. "&password=" .. cred.p
    wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
    return wrk.format()
end
EOF

    cat > "${OUTPUT_DIR}/mixed.lua" << 'EOF'
math.randomseed(os.time())
local users = {
    {u = "testuser", p = "testpass"},
    {u = "user1", p = "pass1"}, {u = "user2", p = "pass2"},
    {u = "user3", p = "pass3"}, {u = "user4", p = "pass4"}
}
local index = 0
function request()
    if math.random() < 0.7 then
        index = index + 1
        if index > 5 then index = 1 end
        local cred = users[index]
        wrk.method = "POST"
        wrk.body = "username=" .. cred.u .. "&password=" .. cred.p
        wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
        return wrk.format()
    else
        wrk.method = "POST"
        wrk.body = "username=bench_"..math.random(100000,9999999).."&password=123456"
        wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
        return wrk.format()
    end
end
EOF
}

# ==================== 压测执行函数 ====================

run_auth_test() {
    local name=$1
    local threads=$2
    local conns=$3
    local duration=$4
    local script=$5
    local api=$6

    echo -e "${PURPLE}[${name}]${NC} ${threads}线程 / ${conns}并发 / ${duration}秒"

    local output=$(wrk -t${threads} -c${conns} -d${duration}s \
        -s "${OUTPUT_DIR}/${script}" \
        "http://${SERVER_HOST}:${SERVER_PORT}/${api}" 2>&1)

    echo "$output"

    local qps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}')
    local latency=$(echo "$output" | grep "Latency" | awk '{print $2}')
    [ -z "$qps" ] && qps="N/A"
    [ -z "$latency" ] && latency="N/A"

    echo -e "${GREEN}QPS: ${qps} | 延迟: ${latency}${NC}"
    echo ""
}

generate_bin_files() {
    echo -e "${BLUE}[准备]${NC} 生成测试文件..."

    mkdir -p "$WWW_DIR"
    declare -A files=(["1mb.bin"]="1" ["10mb.bin"]="10" ["100mb.bin"]="100")

    for file in "${!files[@]}"; do
        local size=${files[$file]}
        echo "  生成 $file (${size}MB)..."
        rm -f "$WWW_DIR/${file}"
        truncate -s "${size}M" "$WWW_DIR/${file}"
    done

    echo -e "${GREEN}[完成]${NC} 测试文件已生成"
}

run_static_test() {
    local name=$1
    local file=$2
    local args=$3

    echo -e "${PURPLE}[${name}]${NC}"

    local output=$(wrk ${args} "http://${SERVER_HOST}:${SERVER_PORT}/${file}" 2>&1)
    echo "$output"

    local qps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}')
    local transfer=$(echo "$output" | grep "Transfer/sec" | awk '{print $2, $3}')
    [ -z "$qps" ] && qps="N/A"
    [ -z "$transfer" ] && transfer="N/A"

    echo -e "${GREEN}QPS: ${qps} | 带宽: ${transfer}${NC}"
    echo ""
}

# ==================== 测试 1: 静态资源压测 ====================

test_static_resources() {
    clear
    print_header "静态资源压测"
    echo ""

    # 1. 系统调优
    optimize_system || return 1

    # 2. 生成测试文件
    generate_bin_files

    # 3. 编译服务器
    echo -e "${BLUE}[编译]${NC} 编译服务器..."
    cd "$SCRIPT_DIR" || return 1
    rm -rf build
    mkdir -p build
    cd build || return 1
    cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
    if ! make -j$(nproc) > /dev/null 2>&1; then
        echo -e "${RED}[错误]${NC} 编译失败"
        return 1
    fi
    cd "$SCRIPT_DIR" || return 1
    echo -e "${GREEN}[完成]${NC} 编译成功"

    # 4. 启动服务器
    start_server "standalone" || return 1
    sleep 2

    # 5. 执行压测
    echo ""
    print_header "开始压测"
    echo ""

    drop_cache
    run_static_test "welcome.html" "welcome.html" "-t12 -c400 -d30s"

    drop_cache
    run_static_test "index.html" "index.html" "-t12 -c400 -d30s"

    drop_cache
    run_static_test "1mb.bin" "1mb.bin" "-t12 -c200 -d30s"

    drop_cache
    run_static_test "10mb.bin" "10mb.bin" "-t12 -c150 -d30s"

    drop_cache
    run_static_test "100mb.bin" "100mb.bin" "-t12 -c100 -d60s --timeout 10s"

    # 6. 清理
    stop_server
    restore_system

    echo ""
    print_header "压测完成"
    pause
}

# ==================== 测试 2: 认证功能压测 ====================

test_authentication() {
    clear
    print_header "认证功能压测"
    echo ""

    # 1. 系统调优
    optimize_system || return 1

    # 2. 准备数据库
    prepare_db_data

    # 3. 检查从库状态
    check_slave_status

    # 4. 创建 Lua 脚本
    create_lua_scripts

    # 5. 启动服务器
    start_server "standalone" || return 1
    sleep 2

    # 6. 执行压测
    echo ""
    print_header "开始压测"
    echo ""

    echo -e "${BLUE}[阶段 1/4]${NC} 预热..."
    run_auth_test "预热-登录" ${WARMUP_THREADS} ${WARMUP_CONNECTIONS} ${WARMUP_DURATION} "login.lua" "login"
    drop_cache

    echo -e "${BLUE}[阶段 2/4]${NC} 标准登录..."
    run_auth_test "标准-登录" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "login_adv.lua" "login"
    drop_cache

    echo -e "${BLUE}[阶段 3/4]${NC} 标准注册..."
    run_auth_test "标准-注册" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "register.lua" "register"
    drop_cache

    echo -e "${BLUE}[阶段 4/4]${NC} 极限登录..."
    run_auth_test "极限-登录" ${EXTREME_THREADS} ${EXTREME_CONNECTIONS} ${EXTREME_DURATION} "login_adv.lua" "login"

    # 7. 清理
    stop_server
    restore_system
    mysql -u"${DB_USER}" -p"${DB_PASS}" "${DB_NAME}" -e "TRUNCATE TABLE users;" 2>/dev/null

    echo ""
    print_header "压测完成"
    pause
}

# ==================== 测试 3: 主从复制压测 ====================

test_replication() {
    clear
    print_header "主从复制压测"
    echo ""

    # 1. 系统调优
    optimize_system || return 1

    # 2. 准备数据库
    prepare_db_data

    # 3. 检查从库状态
    check_slave_status

    # 4. 创建 Lua 脚本
    create_lua_scripts

    # 5. 测试不同模式
    for mode in "standalone" "mysql_replication" "full_replication"; do
        case "$mode" in
            "standalone") mode_name="单机模式" ;;
            "mysql_replication") mode_name="MySQL 主从模式" ;;
            "full_replication") mode_name="MySQL+Redis 主从模式" ;;
        esac

        echo ""
        print_header "测试模式: $mode_name"
        echo ""

        start_server "$mode" || continue
        sleep 2

        echo -e "${BLUE}[预热]${NC}"
        run_auth_test "预热-登录" ${WARMUP_THREADS} ${WARMUP_CONNECTIONS} ${WARMUP_DURATION} "login_adv.lua" "login"
        drop_cache

        echo -e "${BLUE}[标准-登录]${NC}"
        run_auth_test "标准-登录" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "login_adv.lua" "login"
        drop_cache

        echo -e "${BLUE}[标准-注册]${NC}"
        run_auth_test "标准-注册" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "register.lua" "register"
        drop_cache

        echo -e "${BLUE}[混合读写]${NC}"
        run_auth_test "混合-70%读" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "mixed.lua" ""
        drop_cache

        echo -e "${BLUE}[极限-登录]${NC}"
        run_auth_test "极限-登录" ${EXTREME_THREADS} ${EXTREME_CONNECTIONS} ${EXTREME_DURATION} "login_adv.lua" "login"

        stop_server
    done

    # 6. 清理
    restore_system
    mysql -u"${DB_USER}" -p"${DB_PASS}" "${DB_NAME}" -e "TRUNCATE TABLE users;" 2>/dev/null
    redis-cli flushall 2>/dev/null

    echo ""
    print_header "压测完成"
    pause
}

# ==================== 测试 4: 容灾测试 ====================

test_failover() {
    clear
    print_header "容灾测试"
    echo ""

    echo -e "${YELLOW}[警告]${NC} 此测试将停止 MySQL 主库，验证故障转移功能"
    echo -e "${YELLOW}[警告]${NC} 请确保已正确配置 MySQL 主从复制"
    echo ""
    echo -e "${YELLOW}是否继续? (y/n): ${NC}"
    read -r confirm
    if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
        echo "已取消"
        return
    fi

    # 1. 系统调优
    optimize_system || return 1

    # 2. 准备数据库
    prepare_db_data

    # 3. 检查从库状态
    check_slave_status

    # 4. 创建 Lua 脚本
    create_lua_scripts

    # 5. 启动服务器
    start_server "mysql_replication" || return 1
    sleep 2

    # 6. 预热
    echo ""
    print_header "预热"
    echo ""
    run_auth_test "预热" ${WARMUP_THREADS} ${WARMUP_CONNECTIONS} ${WARMUP_DURATION} "login_adv.lua" "login"

    # 7. 启动持续监控
    echo -e "${BLUE}[监控]${NC} 启动持续请求监控..."

    (
        while true; do
            wrk -t2 -c10 -d1s -s "${OUTPUT_DIR}/login_adv.lua" "http://${SERVER_HOST}:${SERVER_PORT}/login" > /dev/null 2>&1
            echo -n "."
        done
    ) &
    MONITOR_PID=$!

    sleep 5

    # 8. 故障注入
    echo ""
    echo -e "${RED}[故障注入]${NC} 停止 MySQL 主库..."
    systemctl stop mysql 2>/dev/null || service mysql stop 2>/dev/null

    FAILOVER_START=$(date +%s)
    echo -e "${BLUE}[监控]${NC} 等待故障转移..."

    failover_done=0
    max_wait=30
    waited=0

    while [ $waited -lt $max_wait ]; do
        sleep 1
        waited=$((waited + 1))

        response=$(curl -s -o /dev/null -w "%{http_code}" --connect-timeout 1 "http://${SERVER_HOST}:${SERVER_PORT}/login" 2>/dev/null || echo "000")

        if [ "$response" = "200" ] || [ "$response" = "401" ]; then
            failover_done=1
            break
        fi

        if [ $((waited % 5)) -eq 0 ]; then
            echo ""
            echo -e "  已等待 ${waited}s (HTTP: $response)"
        fi
    done

    FAILOVER_END=$(date +%s)
    FAILOVER_TIME=$((FAILOVER_END - FAILOVER_START))

    kill $MONITOR_PID 2>/dev/null
    wait $MONITOR_PID 2>/dev/null

    # 9. 输出结果
    echo ""
    print_header "容灾测试结果"
    echo ""

    if [ $failover_done -eq 1 ]; then
        echo -e "${GREEN}[成功]${NC} 故障转移完成"
        echo -e "  故障检测和恢复时间: ${FAILOVER_TIME} 秒"
    else
        echo -e "${RED}[失败]${NC} 故障转移未能在 ${max_wait} 秒内完成"
    fi

    # 10. 恢复服务
    echo ""
    echo -e "${BLUE}[恢复]${NC} 恢复 MySQL 服务..."
    systemctl start mysql 2>/dev/null || service mysql start 2>/dev/null
    sleep 5

    if mysql -u"${DB_USER}" -p"${DB_PASS}" -e "SELECT 1" > /dev/null 2>&1; then
        echo -e "${GREEN}[完成]${NC} MySQL 已恢复"
    else
        echo -e "${YELLOW}[警告]${NC} MySQL 可能未完全恢复"
    fi

    # 11. 清理
    stop_server
    restore_system

    pause
}

# ==================== 测试 5: 完整测试套件 ====================

test_full_suite() {
    echo ""
    echo -e "${YELLOW}[提示]${NC} 完整测试包含所有测试项目，可能需要较长时间"
    echo -e "${YELLOW}[提示]${NC} 包括: 静态资源 + 认证 + 主从复制 + 容灾"
    echo ""
    echo -e "${YELLOW}是否继续? (y/n): ${NC}"
    read -r confirm
    if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
        echo "已取消"
        return
    fi

    test_static_resources
    test_authentication
    test_replication
    test_failover

    echo ""
    print_header "全部测试完成!"
    echo ""
}

# ==================== 主循环 ====================

main() {
    cd "$SCRIPT_DIR" || exit 1

    while true; do
        print_menu
        read -r choice

        case "$choice" in
            1) test_static_resources ;;
            2) test_authentication ;;
            3) test_replication ;;
            4) test_failover ;;
            5) test_full_suite ;;
            0)
                echo -e "${GREEN}再见!${NC}"
                exit 0
                ;;
            *)
                echo -e "${RED}[错误]${NC} 无效选择，请重新输入"
                sleep 1
                ;;
        esac
    done
}

trap 'echo -e "\n${RED}[中断]${NC} 正在清理..."; cleanup_all; exit 1' INT TERM

main
