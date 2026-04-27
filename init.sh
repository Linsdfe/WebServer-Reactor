#!/bin/bash
# ==========================================
# MySQL + Redis 主从复制架构 - 首次初始化脚本
# 用法: bash init_replication.sh
# ==========================================

set -e

DB_USER="root"
DB_PASS="123456"
DB_NAME="webserver_db"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

print_ok()   { echo -e "${GREEN}[OK]${NC} $1"; }
print_err()  { echo -e "${RED}[ERROR]${NC} $1"; }
print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
print_step() { echo -e "\n${CYAN}========================================${NC}\n${CYAN}  $1${NC}\n${CYAN}========================================${NC}"; }

# ==================== Step 1: Check prerequisites ====================

print_step "Step 1: 检查前置条件"

if ! mysql -u"${DB_USER}" -p"${DB_PASS}" -e "SELECT 1" &>/dev/null; then
    print_err "MySQL 主库未运行或密码不正确"
    exit 1
fi
print_ok "MySQL 主库连接正常"

if ! redis-cli -p 6379 PING 2>/dev/null | grep -q PONG; then
    print_err "Redis 主库未运行"
    exit 1
fi
print_ok "Redis 主库连接正常"

# ==================== Step 2: Create directories ====================

print_step "Step 2: 创建数据目录"

mkdir -p /tmp/mysql-slave1 /tmp/mysql-slave2
mkdir -p /tmp/mysql-slave1-run /tmp/mysql-slave2-run
mkdir -p /tmp/mysql-slave1-log /tmp/mysql-slave2-log
mkdir -p /tmp/redis-slave1-data /tmp/redis-slave2-data
print_ok "所有目录已创建"

# ==================== Step 3: Create replication user ====================

print_step "Step 3: 创建 MySQL 复制用户"

mysql -u"${DB_USER}" -p"${DB_PASS}" -e "
    CREATE USER IF NOT EXISTS 'repl_user'@'127.0.0.1' IDENTIFIED WITH mysql_native_password BY 'repl_pass123';
    GRANT REPLICATION SLAVE ON *.* TO 'repl_user'@'127.0.0.1';
    CREATE USER IF NOT EXISTS 'repl_user'@'localhost' IDENTIFIED WITH mysql_native_password BY 'repl_pass123';
    GRANT REPLICATION SLAVE ON *.* TO 'repl_user'@'localhost';
    CREATE USER IF NOT EXISTS 'repl_user'@'%' IDENTIFIED WITH mysql_native_password BY 'repl_pass123';
    GRANT REPLICATION SLAVE ON *.* TO 'repl_user'@'%';
    FLUSH PRIVILEGES;
" 2>/dev/null
print_ok "复制用户 repl_user 已创建"

# ==================== Step 4: Dump master data ====================

print_step "Step 4: 导出主库数据"

mysqldump -u"${DB_USER}" -p"${DB_PASS}" --master-data=2 --single-transaction --routines --triggers --databases ${DB_NAME} > /tmp/master_dump.sql 2>/dev/null
print_ok "主库数据已导出到 /tmp/master_dump.sql"

# ==================== Step 5: Initialize slave data directories ====================

print_step "Step 5: 初始化从库数据目录"

if [ -f /tmp/mysql-slave1/ibdata1 ]; then
    print_info "从库1 数据目录已存在，跳过初始化"
else
    mysqld --initialize-insecure --datadir=/tmp/mysql-slave1 --log-error=/tmp/mysql-slave1-log/error.log
    print_ok "从库1 数据目录已初始化"
fi

if [ -f /tmp/mysql-slave2/ibdata1 ]; then
    print_info "从库2 数据目录已存在，跳过初始化"
else
    mysqld --initialize-insecure --datadir=/tmp/mysql-slave2 --log-error=/tmp/mysql-slave2-log/error.log
    print_ok "从库2 数据目录已初始化"
fi

# ==================== Step 6: Start MySQL slaves ====================

print_step "Step 6: 启动 MySQL 从库"

mysqld \
    --port=3307 --server-id=2 --datadir=/tmp/mysql-slave1 \
    --socket=/tmp/mysql-slave1-run/mysqld.sock \
    --pid-file=/tmp/mysql-slave1-run/mysqld.pid \
    --log-error=/tmp/mysql-slave1-log/error.log \
    --relay-log=/tmp/mysql-slave1-log/relay-bin \
    --log-bin=/tmp/mysql-slave1-log/mysql-bin \
    --binlog-format=ROW --read-only=ON --super-read-only=ON \
    --bind-address=0.0.0.0 --mysqlx=0 --max-connections=300 &
print_info "从库1 启动中..."

mysqld \
    --port=3308 --server-id=3 --datadir=/tmp/mysql-slave2 \
    --socket=/tmp/mysql-slave2-run/mysqld.sock \
    --pid-file=/tmp/mysql-slave2-run/mysqld.pid \
    --log-error=/tmp/mysql-slave2-log/error.log \
    --relay-log=/tmp/mysql-slave2-log/relay-bin \
    --log-bin=/tmp/mysql-slave2-log/mysql-bin \
    --binlog-format=ROW --read-only=ON --super-read-only=ON \
    --bind-address=0.0.0.0 --mysqlx=0 --max-connections=300 &
print_info "从库2 启动中..."

print_info "等待从库就绪..."
for i in $(seq 1 30); do
    if mysql -u root -P 3307 -h 127.0.0.1 -e "SELECT 1" &>/dev/null && \
       mysql -u root -P 3308 -h 127.0.0.1 -e "SELECT 1" &>/dev/null; then
        print_ok "两个从库均已就绪"
        break
    fi
    if [ $i -eq 30 ]; then
        print_err "从库启动超时"
        exit 1
    fi
    sleep 2
done

# ==================== Step 7: Configure slaves ====================

print_step "Step 7: 配置从库用户和数据"

for port in 3307 3308; do
    mysql -u root -P $port -h 127.0.0.1 -e "SET GLOBAL super_read_only=OFF; SET GLOBAL read_only=OFF;" 2>/dev/null
    mysql -u root -P $port -h 127.0.0.1 -e "
        ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '${DB_PASS}';
        CREATE USER IF NOT EXISTS 'root'@'%' IDENTIFIED WITH mysql_native_password BY '${DB_PASS}';
        GRANT ALL PRIVILEGES ON *.* TO 'root'@'%' WITH GRANT OPTION;
        FLUSH PRIVILEGES;
    " 2>/dev/null
    mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 -e "CREATE DATABASE IF NOT EXISTS ${DB_NAME};" 2>/dev/null
    mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 ${DB_NAME} < /tmp/master_dump.sql 2>/dev/null
    print_ok "从库 (port $port) 用户和数据配置完成"
done

# ==================== Step 8: Configure replication ====================

print_step "Step 8: 配置主从复制"

MASTER_STATUS=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -e "SHOW MASTER STATUS\G" 2>/dev/null)
MASTER_LOG_FILE=$(echo "$MASTER_STATUS" | grep "File:" | awk '{print $2}')
MASTER_LOG_POS=$(echo "$MASTER_STATUS" | grep "Position:" | awk '{print $2}')
print_info "主库位置: ${MASTER_LOG_FILE} / ${MASTER_LOG_POS}"

for port in 3307 3308; do
    mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 -e "
        STOP SLAVE;
        CHANGE MASTER TO
            MASTER_HOST='127.0.0.1',
            MASTER_PORT=3306,
            MASTER_USER='repl_user',
            MASTER_PASSWORD='repl_pass123',
            MASTER_LOG_FILE='${MASTER_LOG_FILE}',
            MASTER_LOG_POS=${MASTER_LOG_POS};
        START SLAVE;
    " 2>/dev/null
    mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 -e "SET GLOBAL read_only=ON; SET GLOBAL super_read_only=ON;" 2>/dev/null
    print_ok "从库 (port $port) 复制已配置"
done

# ==================== Step 9: Start Redis slaves ====================

print_step "Step 9: 启动 Redis 从库"

redis-server --port 6380 --daemonize yes --pidfile /tmp/redis-slave1.pid \
    --logfile /tmp/redis-slave1.log --dir /tmp/redis-slave1-data \
    --replicaof 127.0.0.1 6379 --protected-mode no --appendonly yes
print_ok "Redis 从库1 (port 6380) 已启动"

redis-server --port 6381 --daemonize yes --pidfile /tmp/redis-slave2.pid \
    --logfile /tmp/redis-slave2.log --dir /tmp/redis-slave2-data \
    --replicaof 127.0.0.1 6379 --protected-mode no --appendonly yes
print_ok "Redis 从库2 (port 6381) 已启动"

# ==================== Step 10: Verify ====================

print_step "Step 10: 验证"

sleep 3

echo -e "${YELLOW}MySQL 主从复制状态:${NC}"
for port in 3307 3308; do
    local_status=$(mysql -u"${DB_USER}" -p"${DB_PASS}" -P $port -h 127.0.0.1 -e "SHOW SLAVE STATUS\G" 2>/dev/null)
    io=$(echo "$local_status" | grep "Slave_IO_Running:" | awk '{print $2}')
    sql=$(echo "$local_status" | grep "Slave_SQL_Running:" | awk '{print $2}')
    lag=$(echo "$local_status" | grep "Seconds_Behind_Master:" | awk '{print $2}')
    echo "  从库 (port $port): IO=$io SQL=$sql Lag=${lag}s"
done

echo ""
echo -e "${YELLOW}Redis 主从复制状态:${NC}"
redis-cli -p 6379 info replication 2>/dev/null | grep -E "role|connected_slaves"
for port in 6380 6381; do
    redis-cli -p $port info replication 2>/dev/null | grep -E "role|master_link_status"
done

echo ""
print_ok "初始化完成！使用以下命令管理服务:"
echo "  ./manage_replication.sh start   - 启动从库"
echo "  ./manage_replication.sh stop    - 停止从库"
echo "  ./manage_replication.sh status  - 查看状态"
echo "  ./test_replication.sh           - 测试主从复制"

echo ""
echo "保持 MySQL 从库进程运行中... (Ctrl+C 退出)"
wait
