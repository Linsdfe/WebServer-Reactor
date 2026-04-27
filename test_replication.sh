#!/bin/bash
# ==========================================
# MySQL + Redis 主从复制功能测试脚本
# ==========================================

DB_USER="root"
DB_PASS="123456"
DB_NAME="webserver_db"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

PASS=0
FAIL=0

print_ok()   { echo -e "${GREEN}[PASS]${NC} $1"; PASS=$((PASS + 1)); }
print_err()  { echo -e "${RED}[FAIL]${NC} $1"; FAIL=$((FAIL + 1)); }
print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
print_section() { echo -e "\n${CYAN}========================================${NC}\n${CYAN}  $1${NC}\n${CYAN}========================================${NC}"; }

# ==================== MySQL Tests ====================

test_mysql_connectivity() {
    print_section "MySQL 连接测试"

    if mysql -u"${DB_USER}" -p"${DB_PASS}" -e "SELECT 1" &>/dev/null; then
        print_ok "MySQL 主库 (port 3306) 连接成功"
    else
        print_err "MySQL 主库 (port 3306) 连接失败"
    fi

    for port in 3307 3308; do
        if mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 -e "SELECT 1" &>/dev/null; then
            print_ok "MySQL 从库 (port $port) 连接成功"
        else
            print_err "MySQL 从库 (port $port) 连接失败"
        fi
    done
}

test_mysql_replication_status() {
    print_section "MySQL 主从复制状态测试"

    for port in 3307 3308; do
        local status=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 -e "SHOW SLAVE STATUS\G" 2>/dev/null)
        local io=$(echo "$status" | grep "Slave_IO_Running:" | awk '{print $2}')
        local sql=$(echo "$status" | grep "Slave_SQL_Running:" | awk '{print $2}')
        local lag=$(echo "$status" | grep "Seconds_Behind_Master:" | awk '{print $2}')

        if [ "$io" = "Yes" ] && [ "$sql" = "Yes" ]; then
            print_ok "从库 (port $port) IO 和 SQL 线程正常运行 (Lag: ${lag}s)"
        else
            print_err "从库 (port $port) 复制异常 (IO: $io, SQL: $sql)"
        fi
    done
}

test_mysql_data_sync() {
    print_section "MySQL 数据同步测试"

    local test_id=$(date +%s)
    local test_value="repl_test_${test_id}"

    mysql -u"${DB_USER}" -p"${DB_PASS}" "${DB_NAME}" -e "
        INSERT INTO users (username, password) VALUES ('${test_value}', '__test_only_placeholder__') ON DUPLICATE KEY UPDATE password='__test_only_placeholder__';
    " 2>/dev/null

    if [ $? -eq 0 ]; then
        print_ok "主库写入测试数据成功: ${test_value}"
    else
        print_err "主库写入测试数据失败"
        return
    fi

    sleep 2

    for port in 3307 3308; do
        local result=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 "${DB_NAME}" -e "SELECT username FROM users WHERE username='${test_value}';" 2>/dev/null | tail -n +2)
        if [ "$result" = "${test_value}" ]; then
            print_ok "从库 (port $port) 数据同步成功: ${test_value}"
        else
            print_err "从库 (port $port) 数据同步失败 (expected: ${test_value}, got: ${result})"
        fi
    done

    mysql -u"${DB_USER}" -p"${DB_PASS}" "${DB_NAME}" -e "DELETE FROM users WHERE username='${test_value}';" 2>/dev/null
    print_info "清理测试数据"
}

test_mysql_read_only() {
    print_section "MySQL 从库只读测试"

    for port in 3307 3308; do
        local result=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 -e "SHOW VARIABLES LIKE 'read_only';" 2>/dev/null | tail -n +2 | awk '{print $2}')
        if [ "$result" = "ON" ]; then
            print_ok "从库 (port $port) read_only = ON"
        else
            print_err "从库 (port $port) read_only = $result (expected: ON)"
        fi
    done
}

test_mysql_write_to_slave_blocked() {
    print_section "MySQL 从库写入拒绝测试"

    for port in 3307 3308; do
        local result=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 "${DB_NAME}" -e "INSERT INTO users (username, password) VALUES ('should_fail', '__test__');" 2>&1)
        if echo "$result" | grep -q "read-only"; then
            print_ok "从库 (port $port) 正确拒绝写入操作"
        else
            print_err "从库 (port $port) 未拒绝写入操作"
        fi
    done
}

# ==================== Redis Tests ====================

test_redis_connectivity() {
    print_section "Redis 连接测试"

    if redis-cli -p 6379 PING 2>/dev/null | grep -q PONG; then
        print_ok "Redis 主库 (port 6379) 连接成功"
    else
        print_err "Redis 主库 (port 6379) 连接失败"
    fi

    for port in 6380 6381; do
        if redis-cli -p $port PING 2>/dev/null | grep -q PONG; then
            print_ok "Redis 从库 (port $port) 连接成功"
        else
            print_err "Redis 从库 (port $port) 连接失败"
        fi
    done
}

test_redis_replication_status() {
    print_section "Redis 主从复制状态测试"

    local master_info=$(redis-cli -p 6379 info replication 2>/dev/null)
    local connected_slaves=$(echo "$master_info" | grep "^connected_slaves:" | cut -d: -f2 | tr -d '\r')

    if [ "$connected_slaves" = "2" ]; then
        print_ok "Redis 主库有 2 个从库连接"
    else
        print_err "Redis 主库有 ${connected_slaves} 个从库连接 (expected: 2)"
    fi

    for port in 6380 6381; do
        local slave_info=$(redis-cli -p $port info replication 2>/dev/null)
        local role=$(echo "$slave_info" | grep "^role:" | cut -d: -f2 | tr -d '\r')
        local link=$(echo "$slave_info" | grep "^master_link_status:" | cut -d: -f2 | tr -d '\r')

        if [ "$role" = "slave" ] && [ "$link" = "up" ]; then
            print_ok "Redis 从库 (port $port) 角色正确，主库连接正常"
        else
            print_err "Redis 从库 (port $port) 状态异常 (role: $role, link: $link)"
        fi
    done
}

test_redis_data_sync() {
    print_section "Redis 数据同步测试"

    local test_key="repl_test_$(date +%s)"
    local test_value="hello_replication"

    redis-cli -p 6379 SET "${test_key}" "${test_value}" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_ok "Redis 主库写入测试数据成功: ${test_key}=${test_value}"
    else
        print_err "Redis 主库写入测试数据失败"
        return
    fi

    sleep 1

    for port in 6380 6381; do
        local result=$(redis-cli -p $port GET "${test_key}" 2>/dev/null)
        if [ "$result" = "${test_value}" ]; then
            print_ok "Redis 从库 (port $port) 数据同步成功: ${test_key}=${test_value}"
        else
            print_err "Redis 从库 (port $port) 数据同步失败 (expected: ${test_value}, got: ${result})"
        fi
    done

    redis-cli -p 6379 DEL "${test_key}" 2>/dev/null
    print_info "清理测试数据"
}

test_redis_write_to_slave_blocked() {
    print_section "Redis 从库写入拒绝测试"

    for port in 6380 6381; do
        local result=$(redis-cli -p $port SET "should_fail_key" "value" 2>&1)
        if echo "$result" | grep -qi "READONLY\|read-only"; then
            print_ok "Redis 从库 (port $port) 正确拒绝写入操作"
        else
            print_err "Redis 从库 (port $port) 未拒绝写入操作 (result: $result)"
        fi
    done
}

# ==================== Summary ====================

print_summary() {
    print_section "测试结果汇总"

    local total=$((PASS + FAIL))
    echo -e "  总计: ${total}  通过: ${GREEN}${PASS}${NC}  失败: ${RED}${FAIL}${NC}"

    if [ $FAIL -eq 0 ]; then
        echo -e "\n  ${GREEN}所有测试通过！主从复制架构运行正常。${NC}"
    else
        echo -e "\n  ${RED}有 ${FAIL} 个测试失败，请检查配置。${NC}"
    fi
}

# ==================== Main ====================

main() {
    echo ""
    echo -e "${CYAN}╔══════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║  MySQL + Redis 主从复制功能测试          ║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════════╝${NC}"

    test_mysql_connectivity
    test_mysql_replication_status
    test_mysql_data_sync
    test_mysql_read_only
    test_mysql_write_to_slave_blocked

    test_redis_connectivity
    test_redis_replication_status
    test_redis_data_sync
    test_redis_write_to_slave_blocked

    print_summary
}

main "$@"
