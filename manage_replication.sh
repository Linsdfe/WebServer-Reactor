#!/bin/bash
# ==========================================
# MySQL + Redis 主从复制架构管理脚本
# 用法: ./manage_replication.sh [start|stop|status|restart]
# ==========================================

DB_USER="root"
DB_PASS="123456"
DB_NAME="webserver_db"

MYSQL_SLAVE1_PORT=3307
MYSQL_SLAVE2_PORT=3308
REDIS_SLAVE1_PORT=6380
REDIS_SLAVE2_PORT=6381

MYSQL_SLAVE1_DIR="/tmp/mysql-slave1"
MYSQL_SLAVE2_DIR="/tmp/mysql-slave2"
MYSQL_SLAVE1_RUN="/tmp/mysql-slave1-run"
MYSQL_SLAVE2_RUN="/tmp/mysql-slave2-run"
MYSQL_SLAVE1_LOG="/tmp/mysql-slave1-log"
MYSQL_SLAVE2_LOG="/tmp/mysql-slave2-log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

print_ok()   { echo -e "${GREEN}[OK]${NC} $1"; }
print_err()  { echo -e "${RED}[ERROR]${NC} $1"; }
print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }

check_mysql_master() {
    mysql -u"${DB_USER}" -p"${DB_PASS}" -e "SELECT 1" &>/dev/null
}

check_mysql_slave() {
    local port=$1
    mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 -e "SELECT 1" &>/dev/null
}

check_redis_master() {
    redis-cli -p 6379 PING 2>/dev/null | grep -q PONG
}

check_redis_slave() {
    local port=$1
    redis-cli -p $port PING 2>/dev/null | grep -q PONG
}

do_start() {
    echo -e "${CYAN}========================================${NC}"
    echo -e "${CYAN}  启动主从复制架构${NC}"
    echo -e "${CYAN}========================================${NC}"

    if ! check_mysql_master; then
        print_err "MySQL 主库未运行，请先启动 MySQL 服务"
        return 1
    fi
    print_ok "MySQL 主库 (port 3306) 运行中"

    if ! check_redis_master; then
        print_err "Redis 主库未运行，请先启动 Redis 服务"
        return 1
    fi
    print_ok "Redis 主库 (port 6379) 运行中"

    if check_mysql_slave 3307; then
        print_info "MySQL 从库1 (port 3307) 已在运行"
    else
        print_info "启动 MySQL 从库1 (port 3307)..."
        mkdir -p "${MYSQL_SLAVE1_RUN}" "${MYSQL_SLAVE1_LOG}"
        mysqld \
            --port=3307 \
            --server-id=2 \
            --datadir="${MYSQL_SLAVE1_DIR}" \
            --socket="${MYSQL_SLAVE1_RUN}/mysqld.sock" \
            --pid-file="${MYSQL_SLAVE1_RUN}/mysqld.pid" \
            --log-error="${MYSQL_SLAVE1_LOG}/error.log" \
            --relay-log="${MYSQL_SLAVE1_LOG}/relay-bin" \
            --log-bin="${MYSQL_SLAVE1_LOG}/mysql-bin" \
            --binlog-format=ROW \
            --read-only=ON \
            --super-read-only=ON \
            --bind-address=0.0.0.0 \
            --mysqlx=0 \
            --max-connections=300 &
        sleep 5
        if check_mysql_slave 3307; then
            print_ok "MySQL 从库1 (port 3307) 启动成功"
        else
            print_err "MySQL 从库1 (port 3307) 启动失败"
            cat "${MYSQL_SLAVE1_LOG}/error.log" | tail -10
        fi
    fi

    if check_mysql_slave 3308; then
        print_info "MySQL 从库2 (port 3308) 已在运行"
    else
        print_info "启动 MySQL 从库2 (port 3308)..."
        mkdir -p "${MYSQL_SLAVE2_RUN}" "${MYSQL_SLAVE2_LOG}"
        mysqld \
            --port=3308 \
            --server-id=3 \
            --datadir="${MYSQL_SLAVE2_DIR}" \
            --socket="${MYSQL_SLAVE2_RUN}/mysqld.sock" \
            --pid-file="${MYSQL_SLAVE2_RUN}/mysqld.pid" \
            --log-error="${MYSQL_SLAVE2_LOG}/error.log" \
            --relay-log="${MYSQL_SLAVE2_LOG}/relay-bin" \
            --log-bin="${MYSQL_SLAVE2_LOG}/mysql-bin" \
            --binlog-format=ROW \
            --read-only=ON \
            --super-read-only=ON \
            --bind-address=0.0.0.0 \
            --mysqlx=0 \
            --max-connections=300 &
        sleep 5
        if check_mysql_slave 3308; then
            print_ok "MySQL 从库2 (port 3308) 启动成功"
        else
            print_err "MySQL 从库2 (port 3308) 启动失败"
            cat "${MYSQL_SLAVE2_LOG}/error.log" | tail -10
        fi
    fi

    if check_redis_slave 6380; then
        print_info "Redis 从库1 (port 6380) 已在运行"
    else
        print_info "启动 Redis 从库1 (port 6380)..."
        mkdir -p /tmp/redis-slave1-data
        redis-server \
            --port 6380 \
            --daemonize yes \
            --pidfile /tmp/redis-slave1.pid \
            --logfile /tmp/redis-slave1.log \
            --dir /tmp/redis-slave1-data \
            --replicaof 127.0.0.1 6379 \
            --protected-mode no \
            --appendonly yes
        sleep 1
        if check_redis_slave 6380; then
            print_ok "Redis 从库1 (port 6380) 启动成功"
        else
            print_err "Redis 从库1 (port 6380) 启动失败"
        fi
    fi

    if check_redis_slave 6381; then
        print_info "Redis 从库2 (port 6381) 已在运行"
    else
        print_info "启动 Redis 从库2 (port 6381)..."
        mkdir -p /tmp/redis-slave2-data
        redis-server \
            --port 6381 \
            --daemonize yes \
            --pidfile /tmp/redis-slave2.pid \
            --logfile /tmp/redis-slave2.log \
            --dir /tmp/redis-slave2-data \
            --replicaof 127.0.0.1 6379 \
            --protected-mode no \
            --appendonly yes
        sleep 1
        if check_redis_slave 6381; then
            print_ok "Redis 从库2 (port 6381) 启动成功"
        else
            print_err "Redis 从库2 (port 6381) 启动失败"
        fi
    fi

    echo ""
    print_ok "所有服务启动完成"
}

do_stop() {
    echo -e "${CYAN}========================================${NC}"
    echo -e "${CYAN}  停止主从复制架构${NC}"
    echo -e "${CYAN}========================================${NC}"

    print_info "停止 Redis 从库..."
    redis-cli -p 6380 SHUTDOWN NOSAVE 2>/dev/null && print_ok "Redis 从库1 (port 6380) 已停止" || print_info "Redis 从库1 未运行"
    redis-cli -p 6381 SHUTDOWN NOSAVE 2>/dev/null && print_ok "Redis 从库2 (port 6381) 已停止" || print_info "Redis 从库2 未运行"

    print_info "停止 MySQL 从库..."
    mysqladmin -u root -p123456 -P 3307 -h 127.0.0.1 shutdown 2>/dev/null && print_ok "MySQL 从库1 (port 3307) 已停止" || print_info "MySQL 从库1 未运行"
    mysqladmin -u root -p123456 -P 3308 -h 127.0.0.1 shutdown 2>/dev/null && print_ok "MySQL 从库2 (port 3308) 已停止" || print_info "MySQL 从库2 未运行"

    sleep 2
    print_ok "所有从库服务已停止"
    print_info "MySQL 主库和 Redis 主库仍保持运行（需通过 systemctl 管理）"
}

do_status() {
    echo -e "${CYAN}========================================${NC}"
    echo -e "${CYAN}  主从复制架构状态${NC}"
    echo -e "${CYAN}========================================${NC}"

    echo ""
    echo -e "${YELLOW}========== MySQL 实例 ==========${NC}"

    if check_mysql_master; then
        echo -e "${GREEN}[运行]${NC} MySQL 主库 (port 3306)"
        mysql -u"${DB_USER}" -p"${DB_PASS}" -e "SHOW MASTER STATUS;" 2>/dev/null | tail -n +2
    else
        echo -e "${RED}[停止]${NC} MySQL 主库 (port 3306)"
    fi

    for port in 3307 3308; do
        if check_mysql_slave $port; then
            echo -e "${GREEN}[运行]${NC} MySQL 从库 (port $port)"
            local slave_status=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 -e "SHOW SLAVE STATUS\G" 2>/dev/null)
            local io_running=$(echo "$slave_status" | grep "Slave_IO_Running:" | awk '{print $2}')
            local sql_running=$(echo "$slave_status" | grep "Slave_SQL_Running:" | awk '{print $2}')
            local lag=$(echo "$slave_status" | grep "Seconds_Behind_Master:" | awk '{print $2}')
            echo "  IO: $io_running | SQL: $sql_running | Lag: ${lag}s"
        else
            echo -e "${RED}[停止]${NC} MySQL 从库 (port $port)"
        fi
    done

    echo ""
    echo -e "${YELLOW}========== Redis 实例 ==========${NC}"

    if check_redis_master; then
        echo -e "${GREEN}[运行]${NC} Redis 主库 (port 6379)"
        redis-cli -p 6379 info replication 2>/dev/null | grep -E "role|connected_slaves|slave[0-9]"
    else
        echo -e "${RED}[停止]${NC} Redis 主库 (port 6379)"
    fi

    for port in 6380 6381; do
        if check_redis_slave $port; then
            echo -e "${GREEN}[运行]${NC} Redis 从库 (port $port)"
            redis-cli -p $port info replication 2>/dev/null | grep -E "role|master_host|master_port|master_link_status"
        else
            echo -e "${RED}[停止]${NC} Redis 从库 (port $port)"
        fi
    done
}

do_restart() {
    do_stop
    sleep 3
    do_start
}

case "${1}" in
    start)   do_start ;;
    stop)    do_stop ;;
    status)  do_status ;;
    restart) do_restart ;;
    *)
        echo "用法: $0 {start|stop|status|restart}"
        echo ""
        echo "  start   - 启动所有从库实例"
        echo "  stop    - 停止所有从库实例"
        echo "  status  - 查看所有实例状态"
        echo "  restart - 重启所有从库实例"
        exit 1
        ;;
esac
