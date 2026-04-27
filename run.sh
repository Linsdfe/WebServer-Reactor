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

CURRENT_MODE="standalone"

# 压测配置 (基于 12核CPU / 24 IO线程 / 48连接池)
WARMUP_THREADS=6
WARMUP_CONNECTIONS=100
WARMUP_DURATION=10
STD_THREADS=12
STD_CONNECTIONS=300
STD_DURATION=30
HIGH_THREADS=24
HIGH_CONNECTIONS=600
HIGH_DURATION=30
EXTREME_THREADS=48
EXTREME_CONNECTIONS=1000
EXTREME_DURATION=60

# ==================== 辅助函数 ====================

run_sudo() {
    echo "${SUDO_PASS}" | sudo -S "$@" 2>&1
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
    echo "║  6. 选择服务器模式                      ║"
    echo "║  0. 退出 (Exit)                         ║"
    echo "╚══════════════════════════════════════════╝"
    echo -e "${NC}"
    echo -e "${YELLOW}当前服务器模式: ${GREEN}${CURRENT_MODE}${NC}"
    echo -e "${YELLOW}请选择测试类型 [0-6]: ${NC}"
}

pause() {
    echo ""
    echo -e "${YELLOW}按 Enter 键继续...${NC}"
    read -r
}

error_exit() {
    echo ""
    echo -e "${RED}==========================================${NC}"
    echo -e "${RED}  错误: $1${NC}"
    echo -e "${RED}==========================================${NC}"
    echo ""
    pause
    return 1
}

select_server_mode() {
    echo ""
    echo -e "${CYAN}==========================================${NC}"
    echo -e "${CYAN}  选择服务器模式${NC}"
    echo -e "${CYAN}==========================================${NC}"
    echo ""
    echo -e "  ${GREEN}1.${NC} 单机模式 (Standalone)        - 无从库, 读写均走主库"
    echo -e "  ${GREEN}2.${NC} MySQL主从模式 (Replication)  - MySQL从库读, 主库写"
    echo -e "  ${GREEN}3.${NC} 完整主从模式 (Full)           - MySQL+Redis 主从读写分离"
    echo ""
    echo -e "${YELLOW}请选择 [1-3]: ${NC}"
    read -r mode_choice

    case "$mode_choice" in
        1)
            CURRENT_MODE="standalone"
            echo -e "${GREEN}[已切换]${NC} 单机模式"
            ;;
        2)
            CURRENT_MODE="mysql_replication"
            echo -e "${GREEN}[已切换]${NC} MySQL主从模式"
            ;;
        3)
            CURRENT_MODE="full_replication"
            echo -e "${GREEN}[已切换]${NC} 完整主从模式 (MySQL+Redis)"
            ;;
        *)
            echo -e "${RED}[错误]${NC} 无效选择, 保持当前模式: $CURRENT_MODE"
            ;;
    esac
    pause
}

# ==================== 系统调优 ====================

optimize_system() {
    echo -e "${BLUE}[系统调优]${NC}"

    echo -e "${YELLOW}[1/4]${NC} 调整文件描述符限制..."
    ulimit -n 65535 2>/dev/null
    run_sudo bash -c 'ulimit -n 65535' 2>/dev/null
    local cur_limit=$(ulimit -n)
    if [ "$cur_limit" -lt 65535 ]; then
        echo -e "   ${YELLOW}当前限制: $cur_limit (未能调整到65535, 不影响测试)${NC}"
    else
        echo -e "   ${GREEN}当前限制: $cur_limit${NC}"
    fi

    echo -e "${YELLOW}[2/4]${NC} 清空系统缓存..."
    sync
    local drop_result=$(run_sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>&1)
    if echo "$drop_result" | grep -qi "error\|denied\|permission\|拒绝"; then
        echo -e "   ${YELLOW}缓存清理需要sudo权限, 已跳过 (不影响测试)${NC}"
    else
        echo -e "   ${GREEN}缓存已清理${NC}"
    fi
    sleep 1

    echo -e "${YELLOW}[3/4]${NC} 优化网络参数..."
    local net_errors=0
    run_sudo sysctl -w net.core.somaxconn=65535 2>/dev/null || net_errors=$((net_errors + 1))
    run_sudo sysctl -w net.core.netdev_max_backlog=65535 2>/dev/null || net_errors=$((net_errors + 1))
    run_sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535 2>/dev/null || net_errors=$((net_errors + 1))
    run_sudo sysctl -w net.ipv4.tcp_tw_reuse=1 2>/dev/null || net_errors=$((net_errors + 1))
    if [ $net_errors -gt 0 ]; then
        echo -e "   ${YELLOW}部分网络参数调整需要sudo权限, 已跳过 (不影响测试)${NC}"
    else
        echo -e "   ${GREEN}网络参数已优化${NC}"
    fi

    echo -e "${YELLOW}[4/4]${NC} 禁用 ASLR..."
    local aslr_result=$(run_sudo sh -c 'echo 0 > /proc/sys/kernel/randomize_va_space' 2>&1)
    if echo "$aslr_result" | grep -qi "error\|denied\|permission\|拒绝"; then
        echo -e "   ${YELLOW}ASLR调整需要sudo权限, 已跳过 (不影响测试)${NC}"
    else
        echo -e "   ${GREEN}ASLR已禁用${NC}"
    fi

    if ! command -v wrk &> /dev/null; then
        echo -e "${RED}[错误]${NC} 未找到 wrk 压测工具"
        echo -e "${YELLOW}请安装: sudo apt install wrk${NC}"
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
    local mode="${CURRENT_MODE:-standalone}"
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
        error_exit "请先编译项目 (build/bin 目录不存在)"
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
        echo -e "${RED}[错误]${NC} 服务器启动失败, 日志如下:"
        echo -e "${YELLOW}------ server.log ------${NC}"
        cat server.log 2>/dev/null | tail -30
        echo -e "${YELLOW}------------------------${NC}"
        error_exit "服务器启动失败, 请检查上方日志"
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
    echo -e "${BLUE}[数据库]${NC} 清空测试数据..."

    if ! mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P 3306 -e "SELECT 1" &>/dev/null; then
        echo -e "${YELLOW}[警告]${NC} MySQL 连接失败, 跳过数据准备"
        return 0
    fi

    mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P 3306 "${DB_NAME}" -e "TRUNCATE TABLE users;" 2>/dev/null

    redis-cli flushall 2>/dev/null || true

    echo -e "${GREEN}[完成]${NC} 测试数据已清空 (用户将通过注册API创建)"
}

register_test_users() {
    echo -e "${BLUE}[注册]${NC} 通过注册API创建测试用户..."

    local users=("testuser:testpass" "user1:pass1" "user2:pass2" "user3:pass3" "user4:pass4"
                 "user5:pass5" "user6:pass6" "user7:pass7" "user8:pass8"
                 "user9:pass9" "user10:pass10")

    local success=0
    local fail=0
    for entry in "${users[@]}"; do
        local username="${entry%%:*}"
        local password="${entry#*:}"
        local resp=$(curl -s -m 3 -X POST -d "username=${username}&password=${password}" \
            --connect-timeout 2 "http://${SERVER_HOST}:${SERVER_PORT}/register" 2>/dev/null)
        if echo "$resp" | grep -q '"success".*true'; then
            success=$((success + 1))
        else
            fail=$((fail + 1))
        fi
    done

    if [ $fail -eq 0 ]; then
        echo -e "${GREEN}[完成]${NC} ${success} 个测试用户注册成功"
    else
        echo -e "${YELLOW}[警告]${NC} ${success} 成功, ${fail} 失败 (用户可能已存在)"
    fi
}

cleanup_test_data() {
    echo -e "${BLUE}[清理]${NC} 清空测试数据..."
    mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P 3306 "${DB_NAME}" -e "TRUNCATE TABLE users;" 2>/dev/null
    redis-cli flushall 2>/dev/null || true
    echo -e "${GREEN}[完成]${NC} 测试数据已清理"
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
            echo -e "${YELLOW}超时 (不影响测试)${NC}"
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
        if [ -z "$status" ]; then
            local is_readonly=$(mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "SHOW VARIABLES LIKE 'read_only'" 2>/dev/null | tail -1 | awk '{print $2}')
            if [ "$is_readonly" = "OFF" ]; then
                echo -e "${YELLOW}非从库${NC} (read_only=OFF, 可能已被提升为主库)"
            else
                echo -e "${RED}无复制状态${NC} (未配置主从复制)"
            fi
        else
            local io=$(echo "$status" | grep "Slave_IO_Running:" | awk '{print $2}')
            local sql=$(echo "$status" | grep "Slave_SQL_Running:" | awk '{print $2}')
            local lag=$(echo "$status" | grep "Seconds_Behind_Master:" | awk '{print $2}')
            if [ "$io" = "Yes" ] && [ "$sql" = "Yes" ]; then
                echo -e "${GREEN}正常${NC} (IO:$io SQL:$sql Lag:${lag}s)"
            else
                echo -e "${RED}异常${NC} (IO:$io SQL:$sql Lag:${lag}s)"
            fi
        fi
    done

    echo -e "${YELLOW}Redis 从库:${NC}"
    IFS="," read -ra SLAVE_LIST <<< "$REDIS_SLAVES"
    for slave in "${SLAVE_LIST[@]}"; do
        host=$(echo $slave | cut -d':' -f1)
        port=$(echo $slave | cut -d':' -f2)
        echo -n "  $host:$port: "
        local info=$(redis-cli -h "$host" -p "$port" info replication 2>/dev/null)
        local role=$(echo "$info" | grep "^role:" | cut -d: -f2 | tr -d '\r')
        local link=$(echo "$info" | grep "^master_link_status:" | cut -d: -f2 | tr -d '\r')
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

ensure_master_running() {
    if mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P 3306 -e "SELECT 1" &>/dev/null; then
        return 0
    fi

    echo -e "${BLUE}[恢复]${NC} MySQL 主库未运行, 尝试启动..."

    if command -v systemctl &>/dev/null; then
        echo "123456" | sudo -S systemctl start mysql 2>/dev/null
    fi

    local retries=0
    while [ $retries -lt 15 ]; do
        sleep 1
        retries=$((retries + 1))
        if mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P 3306 -e "SELECT 1" &>/dev/null; then
            echo -e "${GREEN}[完成]${NC} MySQL 主库已启动 (等待 ${retries}s)"
            return 0
        fi
    done

    if command -v service &>/dev/null; then
        echo -e "${YELLOW}[重试]${NC} 尝试 service 方式启动..."
        echo "123456" | sudo -S service mysql start 2>/dev/null
        sleep 5
        if mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P 3306 -e "SELECT 1" &>/dev/null; then
            echo -e "${GREEN}[完成]${NC} MySQL 主库已启动"
            return 0
        fi
    fi

    echo -e "${RED}[失败]${NC} 无法启动 MySQL 主库"
    return 1
}

restore_slave_replication() {
    local need_restore=0
    IFS="," read -ra SLAVE_LIST <<< "$MYSQL_SLAVES"
    for slave in "${SLAVE_LIST[@]}"; do
        local host=$(echo $slave | cut -d':' -f1)
        local port=$(echo $slave | cut -d':' -f2)
        local status=$(mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "SHOW SLAVE STATUS\G" 2>/dev/null)
        if [ -z "$status" ]; then
            need_restore=1
            break
        fi
        local io=$(echo "$status" | grep "Slave_IO_Running:" | awk '{print $2}')
        local sql=$(echo "$status" | grep "Slave_SQL_Running:" | awk '{print $2}')
        if [ "$io" != "Yes" ] || [ "$sql" != "Yes" ]; then
            need_restore=1
            break
        fi
    done

    if [ $need_restore -eq 0 ]; then
        return 0
    fi

    echo -e "${BLUE}[恢复]${NC} 从库复制状态异常, 重新配置..."

    if ! ensure_master_running; then
        echo -e "${RED}[失败]${NC} 主库不可用, 无法恢复从库"
        return 1
    fi

    local master_log_file=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P 3306 -e "SHOW MASTER STATUS\G" 2>/dev/null | grep "File:" | awk '{print $2}')
    local master_log_pos=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P 3306 -e "SHOW MASTER STATUS\G" 2>/dev/null | grep "Position:" | awk '{print $2}')

    if [ -z "$master_log_file" ] || [ -z "$master_log_pos" ]; then
        echo -e "${RED}[失败]${NC} 无法获取主库 binlog 位置"
        return 1
    fi

    IFS="," read -ra SLAVE_LIST <<< "$MYSQL_SLAVES"
    for slave in "${SLAVE_LIST[@]}"; do
        local host=$(echo $slave | cut -d':' -f1)
        local port=$(echo $slave | cut -d':' -f2)
        echo -n "  $host:$port: "

        mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "STOP SLAVE;" 2>/dev/null
        mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "RESET SLAVE ALL;" 2>/dev/null
        mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "SET GLOBAL read_only=ON;" 2>/dev/null
        mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "SET GLOBAL super_read_only=ON;" 2>/dev/null
        mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "CHANGE MASTER TO MASTER_HOST='127.0.0.1', MASTER_PORT=3306, MASTER_USER='repl_user', MASTER_PASSWORD='repl_pass', MASTER_LOG_FILE='${master_log_file}', MASTER_LOG_POS=${master_log_pos};" 2>/dev/null
        mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "START SLAVE;" 2>/dev/null

        sleep 2
        local slave_io=$(mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "SHOW SLAVE STATUS\G" 2>/dev/null | grep "Slave_IO_Running:" | awk '{print $2}')
        local slave_sql=$(mysql -h"$host" -P"$port" -u"${DB_USER}" -p"${DB_PASS}" -e "SHOW SLAVE STATUS\G" 2>/dev/null | grep "Slave_SQL_Running:" | awk '{print $2}')
        if [ "$slave_io" = "Yes" ] && [ "$slave_sql" = "Yes" ]; then
            echo -e "${GREEN}已恢复${NC} (IO:$slave_io SQL:$slave_sql)"
        else
            echo -e "${YELLOW}部分恢复${NC} (IO:$slave_io SQL:$slave_sql)"
        fi
    done

    return 0
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
    if ! optimize_system; then
        error_exit "系统调优失败 (通常是因为缺少 wrk 工具)"
        return 1
    fi

    # 2. 生成测试文件
    generate_bin_files

    # 3. 编译服务器
    echo -e "${BLUE}[编译]${NC} 编译服务器..."
    cd "$SCRIPT_DIR" || { error_exit "无法进入项目目录"; return 1; }
    rm -rf build
    mkdir -p build
    cd build || { error_exit "无法创建 build 目录"; return 1; }
    cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
    if ! make -j$(nproc) > /dev/null 2>&1; then
        error_exit "编译失败, 请检查代码"
        return 1
    fi
    cd "$SCRIPT_DIR" || { error_exit "无法返回项目目录"; return 1; }
    echo -e "${GREEN}[完成]${NC} 编译成功"

    # 4. 启动服务器
    if ! start_server; then
        return 1
    fi
    sleep 2

    # 5. 执行压测
    echo ""
    print_header "开始压测"
    echo ""

    drop_cache
    run_static_test "welcome.html" "welcome.html" "-t24 -c600 -d30s"

    drop_cache
    run_static_test "index.html" "index.html" "-t24 -c600 -d30s"

    drop_cache
    run_static_test "1mb.bin" "1mb.bin" "-t24 -c400 -d30s"

    drop_cache
    run_static_test "10mb.bin" "10mb.bin" "-t12 -c200 -d30s"

    drop_cache
    run_static_test "100mb.bin" "100mb.bin" "-t8 -c100 -d60s --timeout 10s"

    # 6. 清理
    stop_server
    restore_system

    echo ""
    print_header "压测完成"
    echo -e "${YELLOW}测试结果如上, 请仔细查看。${NC}"
    pause
}

# ==================== 测试 2: 认证功能压测 ====================

test_authentication() {
    clear
    print_header "认证功能压测"
    echo ""

    # 1. 系统调优
    if ! optimize_system; then
        error_exit "系统调优失败 (通常是因为缺少 wrk 工具)"
        return 1
    fi

    # 2. 准备数据库
    prepare_db_data

    # 3. 检查从库状态
    check_slave_status

    # 4. 创建 Lua 脚本
    create_lua_scripts

    # 5. 启动服务器
    if ! start_server; then
        return 1
    fi
    sleep 2

    # 6. 通过注册API创建测试用户
    register_test_users

    # 7. 执行压测
    echo ""
    print_header "开始压测"
    echo ""

    echo -e "${BLUE}[阶段 1/5]${NC} 预热..."
    run_auth_test "预热-登录" ${WARMUP_THREADS} ${WARMUP_CONNECTIONS} ${WARMUP_DURATION} "login.lua" "login"
    drop_cache

    echo -e "${BLUE}[阶段 2/5]${NC} 标准登录..."
    run_auth_test "标准-登录" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "login_adv.lua" "login"
    drop_cache

    echo -e "${BLUE}[阶段 3/5]${NC} 标准注册..."
    run_auth_test "标准-注册" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "register.lua" "register"
    drop_cache

    echo -e "${BLUE}[阶段 4/5]${NC} 高负载登录..."
    run_auth_test "高负载-登录" ${HIGH_THREADS} ${HIGH_CONNECTIONS} ${HIGH_DURATION} "login_adv.lua" "login"
    drop_cache

    echo -e "${BLUE}[阶段 5/5]${NC} 极限登录..."
    run_auth_test "极限-登录" ${EXTREME_THREADS} ${EXTREME_CONNECTIONS} ${EXTREME_DURATION} "login_adv.lua" "login"

    # 7. 清理
    stop_server
    restore_system
    cleanup_test_data

    echo ""
    print_header "压测完成"
    echo -e "${YELLOW}测试结果如上, 请仔细查看。${NC}"
    pause
}

# ==================== 测试 3: 主从复制压测 ====================

test_replication() {
    clear
    print_header "主从复制压测"
    echo ""

    # 1. 系统调优
    if ! optimize_system; then
        error_exit "系统调优失败 (通常是因为缺少 wrk 工具)"
        return 1
    fi

    # 2. 准备数据库
    prepare_db_data

    # 3. 检查从库状态
    check_slave_status

    # 4. 创建 Lua 脚本
    create_lua_scripts

    # 5. 测试当前模式
    local mode="${CURRENT_MODE:-full_replication}"
    case "$mode" in
        "standalone") mode_name="单机模式" ;;
        "mysql_replication") mode_name="MySQL 主从模式" ;;
        "full_replication") mode_name="MySQL+Redis 主从模式" ;;
    esac

    echo ""
    print_header "测试模式: $mode_name"
    echo ""

    if ! start_server; then
        return 1
    fi
    sleep 2

    register_test_users

    echo -e "${BLUE}[预热]${NC}"
    run_auth_test "预热-登录" ${WARMUP_THREADS} ${WARMUP_CONNECTIONS} ${WARMUP_DURATION} "login_adv.lua" "login"
    drop_cache

    echo -e "${BLUE}[标准-登录]${NC}"
    run_auth_test "标准-登录" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "login_adv.lua" "login"
    drop_cache

    echo -e "${BLUE}[标准-注册]${NC}"
    run_auth_test "标准-注册" ${STD_THREADS} ${STD_CONNECTIONS} ${STD_DURATION} "register.lua" "register"
    drop_cache

    echo -e "${BLUE}[高负载-登录]${NC}"
    run_auth_test "高负载-登录" ${HIGH_THREADS} ${HIGH_CONNECTIONS} ${HIGH_DURATION} "login_adv.lua" "login"
    drop_cache

    echo -e "${BLUE}[混合读写]${NC}"
    run_auth_test "混合-70%读" ${HIGH_THREADS} ${HIGH_CONNECTIONS} ${HIGH_DURATION} "mixed.lua" ""
    drop_cache

    echo -e "${BLUE}[极限-登录]${NC}"
    run_auth_test "极限-登录" ${EXTREME_THREADS} ${EXTREME_CONNECTIONS} ${EXTREME_DURATION} "login_adv.lua" "login"

    stop_server

    # 6. 清理
    restore_system
    cleanup_test_data

    echo ""
    print_header "压测完成"
    echo -e "${YELLOW}测试结果如上, 请仔细查看。${NC}"
    pause
}

# ==================== 测试 4: 容灾测试 ====================

test_failover() {
    clear
    print_header "容灾测试"
    echo ""

    local mode="${CURRENT_MODE:-standalone}"
    if [ "$mode" = "standalone" ]; then
        echo -e "${YELLOW}[提示]${NC} 当前为单机模式, 无从库可进行故障转移"
        echo -e "${YELLOW}[提示]${NC} 请先选择 MySQL主从模式 或 完整主从模式 (菜单选项6)"
        pause
        return
    fi

    echo -e "${YELLOW}[警告]${NC} 此测试将停止 MySQL 主库，验证故障转移功能"
    echo -e "${YELLOW}[警告]${NC} 测试完成后会自动恢复 MySQL 服务"
    echo ""
    echo -e "${YELLOW}是否继续? (y/n): ${NC}"
    read -r confirm
    if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
        echo "已取消"
        return
    fi

    # 0. 环境恢复 (修复上次测试遗留的异常状态)
    echo -e "${BLUE}[预检查]${NC} 检查主从复制环境..."
    if ! ensure_master_running; then
        error_exit "MySQL 主库无法启动, 无法继续测试"
        return 1
    fi
    restore_slave_replication

    # 1. 系统调优
    if ! optimize_system; then
        error_exit "系统调优失败 (通常是因为缺少 wrk 工具)"
        return 1
    fi

    # 2. 准备数据库
    prepare_db_data

    # 3. 检查从库状态
    check_slave_status

    # 4. 创建 Lua 脚本
    create_lua_scripts

    # 5. 启动服务器
    if ! start_server; then
        return 1
    fi
    sleep 2

    # 6. 通过注册API创建测试用户
    register_test_users

    # 7. 预热
    echo ""
    print_header "预热阶段"
    echo ""
    run_auth_test "预热-登录" ${WARMUP_THREADS} ${WARMUP_CONNECTIONS} ${WARMUP_DURATION} "login_adv.lua" "login"

    # 7. 记录故障前状态
    echo ""
    print_header "故障前状态"
    echo ""
    echo -e "${BLUE}[验证]${NC} 确认服务器正常响应..."
    local pre_check=$(curl -s -o /dev/null -w "%{http_code}" -m 3 -X POST -d "username=testuser&password=testpass" --connect-timeout 2 "http://${SERVER_HOST}:${SERVER_PORT}/login" 2>/dev/null)
    [ -z "$pre_check" ] && pre_check="000"
    if [ "$pre_check" = "200" ] || [ "$pre_check" = "401" ]; then
        echo -e "${GREEN}[正常]${NC} 服务器响应正常 (HTTP $pre_check)"
    else
        echo -e "${YELLOW}[注意]${NC} 服务器响应 HTTP $pre_check (非200/401, 但服务可达)"
    fi

    # 8. 故障注入 - 停止 MySQL 主库
    echo ""
    print_header "故障注入"
    echo ""
    echo -e "${RED}[操作]${NC} 停止 MySQL 主库 (port 3306)..."

    local mysql_stopped=0
    if command -v systemctl &>/dev/null; then
        echo "123456" | sudo -S systemctl stop mysql 2>/dev/null && mysql_stopped=1
    fi
    if [ $mysql_stopped -eq 0 ] && command -v service &>/dev/null; then
        echo "123456" | sudo -S service mysql stop 2>/dev/null && mysql_stopped=1
    fi
    if [ $mysql_stopped -eq 0 ]; then
        mysqladmin -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P 3306 shutdown 2>/dev/null && mysql_stopped=1
    fi

    if [ $mysql_stopped -eq 1 ]; then
        echo -e "${GREEN}[完成]${NC} MySQL 主库已停止"
    else
        echo -e "${YELLOW}[警告]${NC} 无法停止 MySQL 主库 (需要sudo权限), 尝试继续..."
    fi

    sleep 2

    # 确认主库已停止
    if mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P 3306 -e "SELECT 1" &>/dev/null; then
        echo -e "${YELLOW}[警告]${NC} MySQL 主库仍在运行, 故障注入可能未生效"
    else
        echo -e "${GREEN}[确认]${NC} MySQL 主库已不可达"
    fi

    # 9. 等待故障转移
    echo ""
    print_header "等待故障转移"
    echo ""
    echo -e "${BLUE}[监控]${NC} 持续发送请求, 等待服务恢复..."
    echo -e "${BLUE}[提示]${NC} 健康检查间隔10秒, 故障检测可能需要等待"
    echo ""

    FAILOVER_START=$(date +%s)
    failover_done=0
    max_wait=45
    waited=0
    last_response=""

    while [ $waited -lt $max_wait ]; do
        sleep 1
        waited=$((waited + 1))

        response=$(curl -s -o /dev/null -w "%{http_code}" -m 3 -X POST -d "username=testuser&password=testpass" --connect-timeout 2 "http://${SERVER_HOST}:${SERVER_PORT}/login" 2>/dev/null)
        [ -z "$response" ] && response="000"

        if [ "$response" != "$last_response" ]; then
            echo ""
            echo -e "  ${waited}s: HTTP $response"
            last_response="$response"
        else
            echo -n "."
        fi

        if [ "$response" != "000" ]; then
            failover_done=1
            break
        fi
    done

    FAILOVER_END=$(date +%s)
    FAILOVER_TIME=$((FAILOVER_END - FAILOVER_START))

    echo ""

    # 10. 验证故障转移后的功能
    echo ""
    print_header "故障转移后验证"
    echo ""

    if [ $failover_done -eq 1 ]; then
        echo -e "${GREEN}[成功]${NC} 故障转移完成!"
        echo -e "  故障检测和恢复时间: ${FAILOVER_TIME} 秒"
        echo ""

        echo -e "${BLUE}[验证1]${NC} 检查新主库连接..."
        local new_master_ok=0
        for port in 3307 3308; do
            if mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P $port -e "SELECT 1" &>/dev/null; then
                local is_readonly=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P $port -e "SHOW VARIABLES LIKE 'read_only'" 2>/dev/null | tail -1 | awk '{print $2}')
                local is_super_readonly=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -h 127.0.0.1 -P $port -e "SHOW VARIABLES LIKE 'super_read_only'" 2>/dev/null | tail -1 | awk '{print $2}')
                echo -e "  从库 port $port: read_only=$is_readonly, super_read_only=$is_super_readonly"
                if [ "$is_readonly" = "OFF" ] || [ -z "$is_readonly" ]; then
                    new_master_ok=1
                fi
            fi
        done
        if [ $new_master_ok -eq 1 ]; then
            echo -e "${GREEN}[通过]${NC} 新主库已可写"
        else
            echo -e "${YELLOW}[注意]${NC} 新主库可能仍为只读状态"
        fi

        echo -e "${BLUE}[验证2]${NC} 发送测试请求..."
        sleep 1
        local post_check=$(curl -s -o /dev/null -w "%{http_code}" -m 3 -X POST -d "username=testuser&password=testpass" --connect-timeout 2 "http://${SERVER_HOST}:${SERVER_PORT}/login" 2>/dev/null)
        [ -z "$post_check" ] && post_check="000"
        if [ "$post_check" = "200" ] || [ "$post_check" = "401" ]; then
            echo -e "${GREEN}[通过]${NC} 请求正常响应 (HTTP $post_check)"
        elif [ "$post_check" != "000" ]; then
            echo -e "${YELLOW}[注意]${NC} 服务器有响应但非预期状态码 (HTTP $post_check)"
        else
            echo -e "${RED}[失败]${NC} 服务器无响应"
        fi

        echo -e "${BLUE}[验证3]${NC} 检查服务器日志..."
        grep -i "故障转移\|failover\|提升从库" server.log 2>/dev/null | tail -5 || echo "  (无故障转移日志)"
    else
        echo -e "${RED}[失败]${NC} 故障转移未能在 ${max_wait} 秒内完成"
        echo ""
        echo -e "${BLUE}[诊断]${NC} 检查服务器日志..."
        echo -e "${YELLOW}------ server.log (最后30行) ------${NC}"
        tail -30 server.log 2>/dev/null
        echo -e "${YELLOW}-----------------------------------${NC}"
    fi

    # 11. 恢复服务 (先停服务器, 再重启主库, 再恢复从库)
    echo ""
    print_header "恢复服务"
    echo ""

    stop_server

    echo -e "${BLUE}[恢复]${NC} 启动 MySQL 主库..."
    if ensure_master_running; then
        echo -e "${GREEN}[完成]${NC} MySQL 主库已恢复 (port 3306)"
        restore_slave_replication
    else
        echo -e "${RED}[失败]${NC} MySQL 主库无法恢复, 从库复制未恢复"
    fi

    restore_system
    cleanup_test_data

    echo ""
    echo -e "${YELLOW}容灾测试结果如上, 请仔细查看。${NC}"
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
    echo -e "${YELLOW}所有测试结果如上, 请仔细查看。${NC}"
    echo ""
    pause
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
            6) select_server_mode ;;
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
