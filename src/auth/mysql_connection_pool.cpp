/**
 * @file mysql_connection_pool.cpp
 * @brief MySQL 连接池实现
 *
 * 核心功能：
 * 1. 连接池管理（预创建、获取、归还、健康检查）
 * 2. 主从库架构（读写分离、轮询负载均衡、故障降级）
 * 3. 数据一致性（半同步复制、复制延迟监控）
 * 4. 容灾能力（主库故障自动转移、数据备份）
 * 5. 监控与告警（连接池状态、复制延迟、健康检查）
 *
 * ==================== 架构设计 ====================
 *
 * 一、连接池结构
 *    MySQLConnectionPool (主库连接池)
 *        ├── connections_ (queue<MySQLConnection*>)  空闲连接队列
 *        ├── active_count_ (atomic<int>)              活跃连接计数
 *        ├── slave_pools_ (vector<SlavePool>)         从库连接池列表
 *        └── mutex_/cv_                              线程同步
 *
 *    SlavePool (从库连接池)
 *        ├── connections_ (queue<MySQLConnection*>)  空闲连接队列
 *        ├── active_count_ (atomic<int>)              活跃连接计数
 *        ├── replication_lag_ms (atomic<int64_t>)   复制延迟
 *        └── is_healthy_ (atomic<bool>)               健康状态
 *
 * 二、读写分离流程
 *    写操作 (INSERT/UPDATE/DELETE)
 *        → GetMasterConnection() → 执行SQL → ReturnMasterConnection()
 *
 *    读操作 (SELECT)
 *        → GetSlaveConnection()
 *            ├── 轮询选择从库 (slave_round_robin_)
 *            ├── 检查从库健康状态 (is_healthy_)
 *            ├── 检查复制延迟 (replication_lag_ms)
 *            ├── 延迟超阈值则跳过，选择下一个从库
 *            └── 所有从库不可用时，降级到主库
 *        → 执行SQL → ReturnSlaveConnection()
 *
 * 三、故障转移流程
 *    主库不可用检测
 *        → GetMasterConnection() 失败
 *        → EnsureMasterAvailable() 检查主库健康
 *        → 主库不健康，触发 PerformFailover()
 *
 *    故障转移步骤
 *        1. 加锁防止并发转移 (failover_in_progress_)
 *        2. SelectNewMaster() 选择复制延迟最小的从库
 *        3. 销毁旧主库的所有连接
 *        4. 更新 host_/port_ 为新主库地址
 *        5. 创建新主库的连接池
 *        6. 从 slave_pools_ 中移除被提升的从库
 *        7. 调用 failover_callback_ 通知上层
 *
 * 四、健康检查线程
 *    定期检查主库和所有从库的连通性
 *    刷新各从库的复制延迟信息
 *    清理主库池中的失效连接
 *    触发 health_alert_callback_ 告警
 */

#include "auth/mysql_connection_pool.h"
#include "monitor/metrics_collector.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <sstream>
#include <cstdlib>

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace reactor {

// ==================== MySQLConnection 封装类 ====================

/**
 * @brief MySQL 连接封装构造函数
 * @param conn 已建立的 MySQL 连接指针
 */
MySQLConnection::MySQLConnection(MYSQL* conn, const std::string& host, int port)
    : conn_(conn), host_(host), port_(port) {}

/**
 * @brief 析构函数：关闭并释放 MySQL 连接
 */
MySQLConnection::~MySQLConnection() {
    if (conn_) {
        mysql_close(conn_);  // 关闭 MySQL 连接
        conn_ = nullptr;
    }
}

/**
 * @brief 获取底层 MySQL 连接指针
 * @return MYSQL* 底层连接指针
 */
MYSQL* MySQLConnection::GetRawConnection() {
    return conn_;
}

/**
 * @brief 检查连接是否有效（通过 ping 命令）
 * @return true 连接有效，false 连接失效或为空
 *
 * 使用 mysql_ping() 检测连接是否存活
 * 如果连接已断开，该函数会尝试重连
 */
bool MySQLConnection::IsValid() {
    if (!conn_) {
        return false;
    }
    return mysql_ping(conn_) == 0;  // 0 表示 ping 成功
}

/**
 * @brief 主动关闭连接
 */
void MySQLConnection::Close() {
    if (conn_) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

// ==================== 辅助函数 ====================

/**
 * @brief 获取 CPU 核心数
 * @return int CPU 核心数
 *
 * Windows: GetSystemInfo() → dwNumberOfProcessors
 * Linux: sysconf(_SC_NPROCESSORS_ONLN)
 */
int GetCpuCoreCount() {
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
#else
    return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

/**
 * @brief 获取硬件支持的线程数（通常等于 CPU 核心数）
 * @return int 线程数
 */
int GetThreadCount() {
    return std::thread::hardware_concurrency();
}

/**
 * @brief 根据系统资源自动计算最优连接池大小
 * @return int 推荐连接池大小
 *
 * 计算公式：min(线程数 * 2, CPU核心数 * 4 + 1)
 *
 * 设计理由：
 * - 线程数 * 2：确保每个线程都能获得连接，避免等待
 * - CPU核心数 * 4 + 1：基于 CPU 计算能力，避免过度竞争
 * - 取最小值：既保证性能，又不会浪费资源
 *
 * 示例：
 * - 24线程 + 4核CPU → min(48, 17) = 17
 * - 24线程 + 8核CPU → min(48, 33) = 33
 * - 24线程 + 16核CPU → min(48, 65) = 48
 */
int CalculateOptimalPoolSize() {
    int cpu_cores = GetCpuCoreCount();
    int thread_count = GetThreadCount();
    int size_by_threads = thread_count * 2;
    int size_by_cpu = cpu_cores * 4 + 1;
    return std::min(size_by_threads, size_by_cpu);
}

// ==================== MySQLConnectionPool 构造函数/析构函数 ====================

/**
 * @brief 构造函数：初始化所有成员变量为默认值
 *
 * 默认值设置：
 * - port_: 3306 (MySQL 默认端口)
 * - pool_size_: 8 (初始值，实际在 Initialize 时计算)
 * - max_idle_time_: 300秒 (连接最大空闲时间)
 * - is_initialized_: false (尚未初始化)
 * - active_count_: 0 (无活跃连接)
 * - slave_round_robin_: 0 (从库轮询计数器初始值)
 * - master_healthy_: false (初始认为主库不健康)
 * - semi_sync_enabled_: false (半同步复制初始关闭)
 * - replication_lag_alert_threshold_ms_: 5000ms (复制延迟告警阈值)
 * - health_check_running_: false (健康检查未运行)
 */
MySQLConnectionPool::MySQLConnectionPool()
    : port_(3306), pool_size_(8), max_idle_time_(300),
      slave_round_robin_(0), is_initialized_(false),
      active_count_(0), master_healthy_(false),
      semi_sync_enabled_(false), semi_sync_timeout_ms_(0),
      failover_in_progress_(false),
      replication_lag_alert_threshold_ms_(5000),
      health_check_running_(false) {}

/**
 * @brief 析构函数：停止健康检查线程，关闭所有连接
 */
MySQLConnectionPool::~MySQLConnectionPool() {
    StopHealthCheck();  // 先停止健康检查线程
    Close();            // 再关闭所有连接
}

// ==================== 初始化函数 ====================

/**
 * @brief 初始化单机模式 MySQL 连接池
 *
 * 初始化步骤：
 * 1. 加互斥锁，保证线程安全
 * 2. 如果已初始化，直接返回（防止重复初始化）
 * 3. 保存连接参数
 * 4. 自动计算或使用指定的连接池大小
 * 5. 预创建连接池大小的连接
 * 6. 更新初始化状态和主库健康状态
 *
 * @param host     MySQL 主机地址
 * @param user     用户名
 * @param password 密码
 * @param database 数据库名
 * @param port     端口号
 * @param pool_size 连接池大小（0 表示自动计算）
 * @param max_idle_time 最大空闲时间（秒）
 */
void MySQLConnectionPool::Initialize(const std::string& host, const std::string& user,
                                   const std::string& password, const std::string& database,
                                   int port, int pool_size, int max_idle_time) {
    std::lock_guard<std::mutex> lock(mutex_);  // 加锁保证线程安全

    if (is_initialized_) {  // 防止重复初始化
        return;
    }

    // ========== 步骤1：保存连接配置 ==========
    host_ = host;
    user_ = user;
    password_ = password;
    database_ = database;
    port_ = port;
    max_idle_time_ = max_idle_time;

    // ========== 步骤2：计算连接池大小 ==========
    if (pool_size <= 0) {
        pool_size_ = CalculateOptimalPoolSize();
    } else {
        pool_size_ = pool_size;
    }

    // ========== 步骤3：预创建连接 ==========
    for (int i = 0; i < pool_size_; ++i) {
        MySQLConnection* conn = CreateConnection(host_, port_, user_, password_, database_);
        if (conn) {
            connections_.push(conn);  // 加入空闲队列
        }
    }

    // ========== 步骤4：更新状态 ==========
    is_initialized_ = true;
    master_healthy_ = !connections_.empty();
    std::cout << "[MySQL] 主库连接池初始化完成: " << connections_.size() << "/" << pool_size_ << " 连接 (" << host_ << ":" << port_ << "/" << database_ << ")" << std::endl;
}

/**
 * @brief 初始化主从模式 MySQL 连接池
 *
 * 初始化步骤：
 * 1. 先初始化主库连接池（调用上面的 Initialize）
 * 2. 如果没有从库配置，输出单机模式日志并返回
 * 3. 遍历从库配置，初始化每个从库的连接池
 * 4. 统计健康的从库数量
 * 5. 向监控组件注册从库数量和健康状态
 * 6. 刷新所有从库的复制延迟信息
 *
 * @param master_config 主库配置
 * @param slave_configs 从库配置列表
 * @param max_idle_time 最大空闲时间（秒）
 */
void MySQLConnectionPool::InitializeWithSlaves(const MySQLNodeConfig& master_config,
                                             const std::vector<MySQLNodeConfig>& slave_configs,
                                             int max_idle_time) {
    // ========== 步骤1：初始化主库 ==========
    Initialize(master_config.host, master_config.user, master_config.password, master_config.database,
              master_config.port, master_config.pool_size, max_idle_time);

    // ========== 步骤2：检查是否有从库 ==========
    if (slave_configs.empty()) {
        std::cout << "[MySQL] 无从库配置, 使用单机模式" << std::endl;
        return;
    }

    // ========== 步骤3：初始化每个从库 ==========
    int healthy_slaves = 0;
    for (size_t i = 0; i < slave_configs.size(); ++i) {
        // 创建从库连接池对象并加入列表
        slave_pools_.emplace_back(std::make_unique<SlavePool>());
        // 初始化该从库
        if (InitializeSlavePool(*slave_pools_[i], slave_configs[i])) {
            healthy_slaves++;
            std::cout << "[MySQL] 从库-" << i << " 连接成功: "
                      << slave_configs[i].host << ":" << slave_configs[i].port << std::endl;
        } else {
            std::cerr << "[MySQL] 从库-" << i << " 连接失败: "
                      << slave_configs[i].host << ":" << slave_configs[i].port << std::endl;
        }
    }

    std::cout << "[MySQL] 主从复制状态: " << healthy_slaves
              << "/" << slave_configs.size() << " 个从库健康" << std::endl;

    // ========== 步骤4：注册监控指标 ==========
    MetricsCollector::Instance().SetMySQLSlaveCount(static_cast<int>(slave_configs.size()));
    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        MetricsCollector::Instance().SetMySQLSlaveHealthy(static_cast<int>(i),
                                                         slave_pools_[i]->is_healthy.load());
    }

    // ========== 步骤5：刷新复制延迟 ==========
    RefreshReplicationLag();
}

/**
 * @brief 初始化单个从库连接池
 *
 * 步骤：
 * 1. 保存从库配置信息
 * 2. 计算连接池大小（与主库相同逻辑）
 * 3. 预创建指定数量的连接
 * 4. 更新健康状态为"有至少一个连接创建成功"
 *
 * @param pool   从库连接池引用
 * @param config 从库配置
 * @return true 至少创建一个连接，false 创建失败
 */
bool MySQLConnectionPool::InitializeSlavePool(SlavePool& pool, const MySQLNodeConfig& config) {
    // ========== 步骤1：保存配置 ==========
    pool.host = config.host;
    pool.port = config.port;
    pool.user = config.user;
    pool.password = config.password;
    pool.database = config.database;
    pool.pool_size = config.pool_size > 0 ? config.pool_size : CalculateOptimalPoolSize();

    // ========== 步骤2：预创建连接 ==========
    std::lock_guard<std::mutex> lock(pool.mutex);
    int created = 0;
    for (int i = 0; i < pool.pool_size; ++i) {
        MySQLConnection* conn = CreateConnection(pool.host, pool.port, pool.user, pool.password, pool.database);
        if (conn && CheckConnection(conn)) {
            pool.connections.push(conn);
            created++;
        } else if (conn) {
            delete conn;  // 连接无效则直接删除
        }
    }

    // ========== 步骤3：更新健康状态 ==========
    pool.is_healthy = (created > 0);  // 有连接创建成功即为健康
    pool.is_initialized = true;
    return pool.is_healthy.load();
}

/**
 * @brief 创建单个 MySQL 连接
 *
 * 步骤：
 * 1. mysql_init() 初始化 MySQL 对象
 * 2. 设置连接超时参数（30秒）
 * 3. 先不带数据库连接，验证能连上
 * 4. 检查数据库是否存在，不存在则自动创建
 * 5. 创建 users 表（如果不存在）
 * 6. 带数据库名建立连接
 * 7. 设置字符集为 utf8mb4（支持中文）
 *
 * @param host     主机地址
 * @param port     端口
 * @param user     用户名
 * @param password 密码
 * @param database 数据库名
 * @return MySQLConnection* 包装后的连接，失败返回 nullptr
 */
MySQLConnection* MySQLConnectionPool::CreateConnection(const std::string& host, int port,
                                                    const std::string& user, const std::string& password,
                                                    const std::string& database) {
    // ========== 步骤1：初始化 ==========
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        std::cerr << "MySQL initialization failed" << std::endl;
        return nullptr;
    }

    // ========== 步骤2：设置连接超时 ==========
    unsigned int connect_timeout = 30;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);

    // ========== 步骤3：先不带数据库连接，验证能连上 ==========
    if (!mysql_real_connect(conn, host.c_str(), user.c_str(),
                           password.c_str(), nullptr,
                           port, nullptr, 0)) {
        std::cerr << "MySQL connection failed: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        return nullptr;
    }

    // ========== 步骤4：检查数据库是否存在，不存在则自动创建 ==========
    std::string db_query = "CREATE DATABASE IF NOT EXISTS `" + database + "` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci";
    if (mysql_query(conn, db_query.c_str())) {
        std::string err_msg = mysql_error(conn);
        if (err_msg.find("read-only") == std::string::npos) {
            std::cerr << "创建数据库失败: " << err_msg << std::endl;
            mysql_close(conn);
            return nullptr;
        }
    }

    // ========== 步骤5：创建 users 表（如果不存在） ==========
    std::string table_query = "CREATE TABLE IF NOT EXISTS `" + database + "`.`users` ("
                             "`id` INT AUTO_INCREMENT PRIMARY KEY, "
                             "`username` VARCHAR(50) NOT NULL UNIQUE, "
                             "`password` VARCHAR(64) NOT NULL, "
                             "`created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
                             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (mysql_query(conn, table_query.c_str())) {
        std::string err_msg = mysql_error(conn);
        if (err_msg.find("read-only") == std::string::npos) {
            std::cerr << "创建users表失败: " << err_msg << std::endl;
            mysql_close(conn);
            return nullptr;
        }
    }

    mysql_select_db(conn, database.c_str());

    // ========== 步骤7：设置字符集 ==========
    mysql_set_character_set(conn, "utf8mb4");

    return new MySQLConnection(conn, host, port);
}

// ==================== 获取连接 ====================

/**
 * @brief 获取主库连接（用于写操作）
 *
 * 获取流程：
 * 1. 检查连接池是否已初始化
 * 2. 检查主库健康状态，不健康则尝试故障转移
 * 3. 加互斥锁，开始等待
 * 4. 队列空且未达池上限：创建新连接
 * 5. 队列空且已达池上限：等待其他连接归还
 * 6. 获取到连接后，检查连接有效性
 * 7. 连接无效则尝试重建，无效且重建失败返回 nullptr
 * 8. 递增活跃计数，返回连接
 *
 * @param timeout_ms 超时时间（毫秒）
 * @return MySQLConnection* 主库连接，超时或失败返回 nullptr
 */
MySQLConnection* MySQLConnectionPool::GetMasterConnection(int timeout_ms) {
    // ========== 步骤1：检查初始化状态 ==========
    if (!is_initialized_) {
        std::cerr << "MySQL connection pool not initialized" << std::endl;
        return nullptr;
    }

    // ========== 步骤2：检查主库健康 ==========
    if (!master_healthy_.load(std::memory_order_relaxed)) {
        if (!EnsureMasterAvailable()) {
            std::cerr << "[MySQL] 主库不可用且故障转移失败" << std::endl;
            return nullptr;
        }
    }

    std::unique_lock<std::mutex> lock(mutex_);  // 加锁

    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);

    // ========== 步骤3：等待可用连接 ==========
    while (connections_.empty()) {
        // 如果未达池上限，创建新连接
        if (active_count_ < pool_size_) {
            MySQLConnection* conn = CreateConnection(host_, port_, user_, password_, database_);
            if (conn) {
                active_count_++;
                return conn;
            }
        }

        // 等待连接归还或超时
        if (cv_.wait_for(lock, timeout) == std::cv_status::timeout) {
            std::cerr << "MySQL master connection timeout" << std::endl;
            return nullptr;
        }

        // 检查总超时
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout) {
            std::cerr << "MySQL master connection timeout" << std::endl;
            return nullptr;
        }
    }

    // ========== 步骤4：从队列获取连接 ==========
    MySQLConnection* conn = connections_.front();
    connections_.pop();

    // ========== 步骤5：检查连接有效性 ==========
    if (!CheckConnection(conn)) {
        delete conn;
        conn = CreateConnection(host_, port_, user_, password_, database_);
        // 重建失败时直接返回 nullptr，不递增 active_count_
        if (!conn) {
            return nullptr;
        }
    }

    active_count_++;
    return conn;
}

/**
 * @brief 获取从库连接（用于读操作）
 *
 * 负载均衡策略：
 * 1. 轮询选择从库 (slave_round_robin_++)
 * 2. 遍历所有从库（最多一圈）
 * 3. 跳过不健康的从库
 * 4. 跳过复制延迟超阈值的从库
 * 5. 从选中的从库池获取连接
 * 6. 所有从库都不可用，降级到主库
 *
 * @param timeout_ms 超时时间（毫秒）
 * @return MySQLConnection* 从库连接，所有从库不可用时返回主库连接
 */
MySQLConnection* MySQLConnectionPool::GetSlaveConnection(int timeout_ms) {
    // ========== 步骤1：无从库时降级到主库 ==========
    if (slave_pools_.empty()) {
        return GetMasterConnection(timeout_ms);
    }

    // ========== 步骤2：轮询选择从库 ==========
    uint32_t start_index = slave_round_robin_.fetch_add(1, std::memory_order_relaxed);
    uint32_t pool_count = static_cast<uint32_t>(slave_pools_.size());

    // ========== 步骤3：遍历所有从库找健康的 ==========
    for (uint32_t i = 0; i < pool_count; ++i) {
        uint32_t idx = (start_index + i) % pool_count;  // 轮询索引
        SlavePool& pool = *slave_pools_[idx];

        // 跳过不健康的从库
        if (!pool.is_healthy.load(std::memory_order_relaxed)) {
            continue;
        }

        // ========== 步骤4：检查复制延迟 ==========
        int64_t lag = pool.replication_lag_ms.load(std::memory_order_relaxed);
        int64_t alert_threshold = replication_lag_alert_threshold_ms_.load(std::memory_order_relaxed);
        // 如果延迟超阈值，跳过该从库（避免读到过期数据）
        if (alert_threshold > 0 && lag > alert_threshold) {
            std::cerr << "[MySQL-Slave-" << idx << "] Replication lag too high: "
                      << lag << "ms (threshold: " << alert_threshold << "ms), skipping" << std::endl;
            continue;
        }

        // ========== 步骤5：从该从库获取连接 ==========
        MySQLConnection* conn = GetSlaveConnectionFromPool(pool, timeout_ms);
        if (conn) {
            return conn;
        }
    }

    // ========== 步骤6：所有从库不可用，降级到主库 ==========
    std::cout << "[MySQL] 所有从库不可用, 回退到主库" << std::endl;
    MetricsCollector::Instance().IncMySQLSlaveFallbacks();
    return GetMasterConnection(timeout_ms);
}

/**
 * @brief 从指定从库池获取连接
 *
 * 流程与 GetMasterConnection 类似，但操作的是从库池
 *
 * @param pool 从库连接池
 * @param timeout_ms 超时时间
 * @return MySQLConnection* 从库连接
 */
MySQLConnection* MySQLConnectionPool::GetSlaveConnectionFromPool(SlavePool& pool, int timeout_ms) {
    std::unique_lock<std::mutex> lock(pool.mutex);
    auto timeout = std::chrono::milliseconds(timeout_ms);

    while (pool.connections.empty()) {
        if (pool.active_count.load() < pool.pool_size) {
            MySQLConnection* conn = CreateConnection(pool.host, pool.port, pool.user, pool.password, pool.database);
            if (conn) {
                pool.active_count++;
                return conn;
            }
        }

        if (pool.cv.wait_for(lock, timeout) == std::cv_status::timeout) {
            return nullptr;
        }
    }

    MySQLConnection* conn = pool.connections.front();
    pool.connections.pop();

    if (!conn->IsValid()) {
        delete conn;
        conn = CreateConnection(pool.host, pool.port, pool.user, pool.password, pool.database);
        if (!conn) {
            return nullptr;
        }
    }

    pool.active_count++;
    return conn;
}

/**
 * @brief 兼容旧接口，默认返回主库连接
 */
MySQLConnection* MySQLConnectionPool::GetConnection(int timeout_ms) {
    return GetMasterConnection(timeout_ms);
}

// ==================== 归还连接 ====================

/**
 * @brief 归还主库连接到连接池
 *
 * 步骤：
 * 1. 空指针检查
 * 2. 加互斥锁
 * 3. 检查连接有效性：
 *    - 有效：放回空闲队列，唤醒等待的线程
 *    - 无效：删除连接
 * 4. 递减活跃计数
 *
 * @param conn 要归还的连接
 */
void MySQLConnectionPool::ReturnMasterConnection(MySQLConnection* conn) {
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (CheckConnection(conn)) {
        connections_.push(conn);
        cv_.notify_one();  // 唤醒一个等待的线程
    } else {
        delete conn;  // 连接无效则删除
    }

    active_count_--;
}

/**
 * @brief 归还从库连接
 *
 * 步骤：
 * 1. 空指针检查
 * 2. 获取连接的源地址信息
 * 3. 遍历所有从库池，匹配地址
 * 4. 匹配到则归还到对应池
 * 5. 未找到匹配则归还到主库
 *
 * @param conn 要归还的连接
 */
void MySQLConnectionPool::ReturnSlaveConnection(MySQLConnection* conn) {
    if (!conn) {
        return;
    }

    std::string conn_server = conn->GetHost() + ":" + std::to_string(conn->GetPort());

    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        SlavePool& pool = *slave_pools_[i];
        if (!pool.is_initialized.load()) continue;

        std::string pool_server = pool.host + ":" + std::to_string(pool.port);
        if (conn_server == pool_server) {
            ReturnSlaveConnectionToPool(conn, pool);
            return;
        }
    }

    ReturnMasterConnection(conn);
}

/**
 * @brief 归还连接到指定从库池
 *
 * @param conn 要归还的连接
 * @param pool 目标从库池
 */
void MySQLConnectionPool::ReturnSlaveConnectionToPool(MySQLConnection* conn, SlavePool& pool) {
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(pool.mutex);
    if (CheckConnection(conn)) {
        pool.connections.push(conn);
        pool.cv.notify_one();
    } else {
        delete conn;
    }
    pool.active_count--;
}

/**
 * @brief 兼容旧接口，归还到主库
 */
void MySQLConnectionPool::ReturnConnection(MySQLConnection* conn) {
    ReturnMasterConnection(conn);
}

// ==================== 关闭和清理 ====================

/**
 * @brief 关闭连接池，释放所有连接
 *
 * 步骤：
 * 1. 停止健康检查线程
 * 2. 加互斥锁
 * 3. 销毁主库池所有连接
 * 4. 销毁所有从库池的所有连接
 * 5. 清空从库池列表
 * 6. 重置状态标志
 */
void MySQLConnectionPool::Close() {
    StopHealthCheck();  // 先停止健康检查

    std::lock_guard<std::mutex> lock(mutex_);

    // 销毁主库连接
    while (!connections_.empty()) {
        MySQLConnection* conn = connections_.front();
        connections_.pop();
        delete conn;
    }

    // 销毁所有从库连接
    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        SlavePool& pool = *slave_pools_[i];
        std::lock_guard<std::mutex> slave_lock(pool.mutex);
        while (!pool.connections.empty()) {
            MySQLConnection* conn = pool.connections.front();
            pool.connections.pop();
            delete conn;
        }
        pool.is_initialized = false;
        pool.is_healthy = false;
    }
    slave_pools_.clear();

    is_initialized_ = false;
    active_count_ = 0;
    master_healthy_ = false;
}

/**
 * @brief 获取主库空闲连接数
 */
int MySQLConnectionPool::GetIdleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

/**
 * @brief 检查连接有效性（委托给 MySQLConnection::IsValid）
 */
bool MySQLConnectionPool::CheckConnection(MySQLConnection* conn) {
    if (!conn) {
        return false;
    }
    return conn->IsValid();
}

/**
 * @brief 清理连接池中的失效连接
 *
 * 遍历空闲队列，删除无效连接
 */
void MySQLConnectionPool::CleanExpiredConnections() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::queue<MySQLConnection*> valid_connections;

    while (!connections_.empty()) {
        MySQLConnection* conn = connections_.front();
        connections_.pop();

        if (CheckConnection(conn)) {
            valid_connections.push(conn);
        } else {
            delete conn;
        }
    }

    connections_ = valid_connections;
}

// ==================== 健康检查 ====================

/**
 * @brief 检查指定从库是否健康
 */
bool MySQLConnectionPool::IsSlaveHealthy(int index) const {
    if (index < 0 || index >= static_cast<int>(slave_pools_.size())) {
        return false;
    }
    return slave_pools_[index]->is_healthy.load(std::memory_order_relaxed);
}

/**
 * @brief 检查主库健康状态
 *
 * 步骤：
 * 1. 尝试获取主库连接（1秒超时）
 * 2. ping 检查连接有效性
 * 3. 归还连接
 * 4. 更新 master_healthy_ 状态
 */
bool MySQLConnectionPool::CheckMasterHealth() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (connections_.empty()) {
        MySQLConnection* conn = CreateConnection(host_, port_, user_, password_, database_);
        if (conn) {
            connections_.push(conn);
            master_healthy_ = true;
            return true;
        }
        master_healthy_ = false;
        return false;
    }

    MySQLConnection* conn = connections_.front();
    connections_.pop();

    bool healthy = CheckConnection(conn);

    if (healthy) {
        connections_.push(conn);
    } else {
        delete conn;
        MySQLConnection* new_conn = CreateConnection(host_, port_, user_, password_, database_);
        if (new_conn) {
            connections_.push(new_conn);
            healthy = true;
        }
    }

    master_healthy_ = healthy;
    return healthy;
}

/**
 * @brief 检查指定从库健康状态
 *
 * 步骤：
 * 1. 获取该从库的连接
 * 2. ping 检查连接有效性
 * 3. 归还连接
 * 4. 更新 is_healthy_ 状态
 */
bool MySQLConnectionPool::CheckSlaveHealth(int index) {
    if (index < 0 || index >= static_cast<int>(slave_pools_.size())) {
        return false;
    }

    SlavePool& pool = *slave_pools_[index];
    MySQLConnection* conn = GetSlaveConnectionFromPool(pool, 1000);
    if (!conn) {
        pool.is_healthy = false;
        return false;
    }

    bool healthy = CheckConnection(conn);
    ReturnSlaveConnectionToPool(conn, pool);
    pool.is_healthy = healthy;

    if (!healthy) {
        std::cerr << "[MySQL-Slave-" << index << "] Health check failed for "
                  << pool.host << ":" << pool.port << std::endl;
    }

    return healthy;
}

// ==================== 数据一致性优化 ====================

/**
 * @brief 启用/禁用半同步复制
 *
 * 半同步复制原理：
 * - 主库执行事务后，等待至少一个从库确认收到 binlog
 * - 从库确认后，主库才提交事务返回客户端
 * - 超时后自动降级为异步复制，避免主库阻塞
 *
 * @param timeout_ms 半同步超时时间（毫秒），0 表示禁用
 */
void MySQLConnectionPool::EnableSemiSync(int timeout_ms) {
    MySQLConnection* conn = GetMasterConnection(3000);
    if (!conn) {
        std::cerr << "[MySQL] 启用半同步复制失败: 无法获取主库连接" << std::endl;
        return;
    }

    MYSQL* mysql = conn->GetRawConnection();
    if (!mysql) {
        ReturnMasterConnection(conn);
        return;
    }

    if (timeout_ms <= 0) {
        // 禁用半同步复制
        mysql_query(mysql, "SET GLOBAL rpl_semi_sync_master_enabled = OFF");
        semi_sync_enabled_ = false;
        semi_sync_timeout_ms_ = 0;
        std::cout << "[MySQL] 半同步复制已禁用" << std::endl;
    } else {
        // 启用半同步复制
        mysql_query(mysql, "SET GLOBAL rpl_semi_sync_master_enabled = ON");
        std::string timeout_sql = "SET GLOBAL rpl_semi_sync_master_timeout = " + std::to_string(timeout_ms);
        mysql_query(mysql, timeout_sql.c_str());
        semi_sync_enabled_ = true;
        semi_sync_timeout_ms_ = timeout_ms;
        std::cout << "[MySQL] 半同步复制已启用, 超时 " << timeout_ms << "ms" << std::endl;
    }

    ReturnMasterConnection(conn);
}

/**
 * @brief 获取指定从库的复制延迟
 */
int64_t MySQLConnectionPool::GetSlaveReplicationLag(int index) const {
    if (index < 0 || index >= static_cast<int>(slave_pools_.size())) {
        return -1;
    }
    return slave_pools_[index]->replication_lag_ms.load(std::memory_order_relaxed);
}

/**
 * @brief 刷新所有从库的复制延迟
 *
 * 步骤：
 * 1. 遍历所有从库
 * 2. 对健康的从库执行 SHOW SLAVE STATUS
 * 3. 提取 Seconds_Behind_Master 字段
 * 4. 如果延迟超阈值，触发告警回调
 */
void MySQLConnectionPool::RefreshReplicationLag() {
    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        SlavePool& pool = *slave_pools_[i];
        if (pool.is_healthy.load(std::memory_order_relaxed)) {
            int64_t lag = QueryReplicationLag(pool);
            pool.replication_lag_ms.store(lag, std::memory_order_relaxed);

            // 检查是否超过告警阈值
            int64_t threshold = replication_lag_alert_threshold_ms_.load(std::memory_order_relaxed);
            if (threshold > 0 && lag > threshold) {
                std::cerr << "[MySQL-ALERT] Slave-" << i << " (" << pool.host << ":" << pool.port
                          << ") replication lag " << lag << "ms exceeds threshold " << threshold << "ms" << std::endl;
                if (health_alert_callback_) {
                    std::string node = pool.host + ":" + std::to_string(pool.port);
                    health_alert_callback_(node, lag <= threshold);
                }
            }
        }
    }
}

/**
 * @brief 查询单个从库的复制延迟
 *
 * 执行 SHOW SLAVE STATUS，解析 Seconds_Behind_Master 字段
 *
 * @param pool 从库连接池
 * @return int64_t 复制延迟（毫秒），-1 表示查询失败
 */
int64_t MySQLConnectionPool::QueryReplicationLag(SlavePool& pool) {
    MySQLConnection* conn = GetSlaveConnectionFromPool(pool, 3000);
    if (!conn) {
        return -1;
    }

    MYSQL* mysql = conn->GetRawConnection();
    if (!mysql) {
        ReturnSlaveConnectionToPool(conn, pool);
        return -1;
    }

    int64_t lag = -1;
    // SHOW SLAVE STATUS 返回从库复制状态
    if (mysql_query(mysql, "SHOW SLAVE STATUS") == 0) {
        MYSQL_RES* result = mysql_store_result(mysql);
        if (result) {
            MYSQL_ROW row = mysql_fetch_row(result);
            if (row) {
                int num_fields = mysql_num_fields(result);
                // 查找 Seconds_Behind_Master 列
                for (int i = 0; i < num_fields; ++i) {
                    MYSQL_FIELD* field = mysql_fetch_field_direct(result, i);
                    if (field && strcmp(field->name, "Seconds_Behind_Master") == 0) {
                        if (row[i]) {
                            lag = std::atoll(row[i]) * 1000;  // 转换为毫秒
                        }
                        break;
                    }
                }
            }
            mysql_free_result(result);
        }
    }

    ReturnSlaveConnectionToPool(conn, pool);
    return lag;
}

// ==================== 容灾能力增强 ====================

/**
 * @brief 设置主库故障转移回调
 */
void MySQLConnectionPool::SetFailoverCallback(FailoverCallback callback) {
    failover_callback_ = std::move(callback);
}

/**
 * @brief 执行主库故障转移
 *
 * 故障转移流程：
 * 1. 加锁防止并发转移（failover_in_progress_ 原子标志）
 * 2. 从已有从库中选择最佳候选（复制延迟最小）
 * 3. 销毁旧主库连接池
 * 4. 将选中的从库提升为新主库（更新 host_/port_）
 * 5. 创建新主库的连接池
 * 6. 从从库列表中移除被提升的从库
 * 7. 调用回调通知上层应用
 *
 * @return true 转移成功，false 转移失败
 */
bool MySQLConnectionPool::PerformFailover() {
    std::lock_guard<std::mutex> lock(failover_mutex_);

    if (failover_in_progress_.exchange(true)) {
        std::cerr << "[MySQL] 故障转移正在进行中" << std::endl;
        return false;
    }

    std::cout << "[MySQL] 开始故障转移..." << std::endl;

    int new_master_idx = SelectNewMaster();
    if (new_master_idx < 0) {
        std::cerr << "[MySQL] 未找到合适的从库进行故障转移" << std::endl;
        failover_in_progress_ = false;
        return false;
    }

    SlavePool& new_master_pool = *slave_pools_[new_master_idx];
    std::string old_master = host_ + ":" + std::to_string(port_);
    std::string new_master = new_master_pool.host + ":" + std::to_string(new_master_pool.port);

    std::cout << "[MySQL] 提升从库-" << new_master_idx << " (" << new_master << ") 为主库" << std::endl;

    // ========== 步骤3：在新主库上关闭只读模式并停止复制 ==========
    {
        MYSQL* raw = mysql_init(nullptr);
        if (raw) {
            if (mysql_real_connect(raw, new_master_pool.host.c_str(),
                                   new_master_pool.user.empty() ? user_.c_str() : new_master_pool.user.c_str(),
                                   new_master_pool.password.empty() ? password_.c_str() : new_master_pool.password.c_str(),
                                   new_master_pool.database.empty() ? database_.c_str() : new_master_pool.database.c_str(),
                                   new_master_pool.port, nullptr, 0)) {
                mysql_query(raw, "STOP SLAVE");
                mysql_query(raw, "RESET SLAVE ALL");
                mysql_query(raw, "SET GLOBAL super_read_only=OFF");
                mysql_query(raw, "SET GLOBAL read_only=OFF");
                std::cout << "[MySQL] 新主库已关闭只读模式并停止复制" << std::endl;
            } else {
                std::cerr << "[MySQL] 连接新主库失败: " << mysql_error(raw) << std::endl;
            }
            mysql_close(raw);
        }
    }

    // ========== 步骤4：销毁旧主库连接 ==========
    {
        std::lock_guard<std::mutex> pool_lock(mutex_);
        while (!connections_.empty()) {
            MySQLConnection* conn = connections_.front();
            connections_.pop();
            delete conn;
        }
    }

    // ========== 步骤5：更新主库地址 ==========
    host_ = new_master_pool.host;
    port_ = new_master_pool.port;

    // ========== 步骤6：创建新主库连接池 ==========
    {
        std::lock_guard<std::mutex> pool_lock(mutex_);
        for (int i = 0; i < pool_size_; ++i) {
            MySQLConnection* conn = CreateConnection(host_, port_, user_, password_, database_);
            if (conn) {
                connections_.push(conn);
            }
        }
        master_healthy_ = !connections_.empty();
    }

    // ========== 步骤7：从从库列表移除被提升的从库 ==========
    slave_pools_.erase(slave_pools_.begin() + new_master_idx);

    // ========== 步骤8：通知上层 ==========
    if (failover_callback_) {
        failover_callback_(old_master, new_master);
    }

    std::cout << "[MySQL] 故障转移完成: " << old_master << " -> " << new_master << std::endl;
    failover_in_progress_ = false;
    return true;
}

/**
 * @brief 确保主库可用，不可用时触发故障转移
 *
 * 检查流程：
 * 1. 如果 master_healthy_ 为 true，直接返回
 * 2. 否则调用 CheckMasterHealth() 再检查一次
 * 3. 如果仍不健康，调用 PerformFailover()
 */
bool MySQLConnectionPool::EnsureMasterAvailable() {
    if (master_healthy_.load(std::memory_order_relaxed)) {
        return true;
    }

    if (CheckMasterHealth()) {
        return true;
    }

    std::cerr << "[MySQL] 主库不可用, 尝试故障转移..." << std::endl;
    return PerformFailover();
}

/**
 * @brief 选择最佳从库作为新主库
 *
 * 选择策略：选择复制延迟最小的健康从库
 *
 * @return int 选中的从库索引，-1 表示没有合适的从库
 */
int MySQLConnectionPool::SelectNewMaster() {
    int best_idx = -1;
    int64_t min_lag = INT64_MAX;
    int fallback_idx = -1;

    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        SlavePool& pool = *slave_pools_[i];

        if (!pool.is_healthy.load(std::memory_order_relaxed)) {
            continue;
        }

        int64_t lag = pool.replication_lag_ms.load(std::memory_order_relaxed);

        if (lag >= 0 && lag < min_lag) {
            min_lag = lag;
            best_idx = static_cast<int>(i);
        }

        if (fallback_idx < 0) {
            fallback_idx = static_cast<int>(i);
        }
    }

    if (best_idx >= 0) {
        return best_idx;
    }

    if (fallback_idx >= 0) {
        std::cout << "[MySQL] 无有效延迟数据, 选择第一个健康从库作为新主库" << std::endl;
        return fallback_idx;
    }

    return -1;
}

/**
 * @brief 备份数据库
 *
 * 使用 mysqldump 命令导出整个数据库
 *
 * @param backup_path 备份文件路径
 * @return true 备份成功，false 备份失败
 */
bool MySQLConnectionPool::BackupDatabase(const std::string& backup_path) {
    std::string safe_path = backup_path;
    for (char& c : safe_path) {
        if (c == ';' || c == '|' || c == '&' || c == '`' || c == '$' || c == '(' || c == ')' || c == '<' || c == '>') {
            c = '_';
        }
    }

    std::ostringstream cmd;
    cmd << "mysqldump -h " << host_ << " -P " << port_
        << " -u " << user_ << " --password=" << password_
        << " " << database_ << " --result-file=" << safe_path;

    int ret = std::system(cmd.str().c_str());
    if (ret == 0) {
        std::cout << "[MySQL] 数据库备份完成: " << safe_path << std::endl;
        return true;
    } else {
        std::cerr << "[MySQL] 数据库备份失败, 返回码: " << ret << std::endl;
        return false;
    }
}

// ==================== 监控与告警 ====================

/**
 * @brief 获取主库连接池统计信息
 */
PoolStats MySQLConnectionPool::GetMasterPoolStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PoolStats stats;
    stats.idle_count = static_cast<int>(connections_.size());
    stats.active_count = active_count_.load(std::memory_order_relaxed);
    stats.pool_size = pool_size_;
    stats.is_healthy = master_healthy_.load(std::memory_order_relaxed);
    return stats;
}

/**
 * @brief 获取指定从库连接池统计信息
 */
PoolStats MySQLConnectionPool::GetSlavePoolStats(int index) const {
    PoolStats stats = {0, 0, 0, false};
    if (index < 0 || index >= static_cast<int>(slave_pools_.size())) {
        return stats;
    }

    const SlavePool& pool = *slave_pools_[index];
    std::lock_guard<std::mutex> lock(pool.mutex);
    stats.idle_count = static_cast<int>(pool.connections.size());
    stats.active_count = pool.active_count.load(std::memory_order_relaxed);
    stats.pool_size = pool.pool_size;
    stats.is_healthy = pool.is_healthy.load(std::memory_order_relaxed);
    return stats;
}

/**
 * @brief 获取所有从库的复制延迟
 */
std::vector<int64_t> MySQLConnectionPool::GetAllReplicationLags() const {
    std::vector<int64_t> lags;
    lags.reserve(slave_pools_.size());
    for (size_t i = 0; i < slave_pools_.size(); ++i) {
        lags.push_back(slave_pools_[i]->replication_lag_ms.load(std::memory_order_relaxed));
    }
    return lags;
}

/**
 * @brief 设置复制延迟告警阈值
 */
void MySQLConnectionPool::SetReplicationLagAlert(int64_t threshold_ms) {
    replication_lag_alert_threshold_ms_ = threshold_ms;
    std::cout << "[MySQL] 复制延迟告警阈值: " << threshold_ms << "ms" << std::endl;
}

/**
 * @brief 设置健康检查告警回调
 */
void MySQLConnectionPool::SetHealthAlertCallback(HealthAlertCallback callback) {
    health_alert_callback_ = std::move(callback);
}

/**
 * @brief 启动后台健康检查线程
 *
 * @param interval_seconds 检查间隔（秒）
 */
void MySQLConnectionPool::StartHealthCheck(int interval_seconds) {
    if (health_check_running_.load()) {
        return;
    }

    health_check_running_ = true;
    health_check_thread_ = std::thread(&MySQLConnectionPool::HealthCheckLoop, this, interval_seconds);
    std::cout << "[MySQL] 健康检查已启动, 间隔 " << interval_seconds << "s" << std::endl;
}

/**
 * @brief 停止后台健康检查线程
 */
void MySQLConnectionPool::StopHealthCheck() {
    health_check_running_ = false;
    if (health_check_thread_.joinable()) {
        health_check_thread_.join();
    }
}

/**
 * @brief 健康检查线程主循环
 *
 * 每隔 interval_seconds 执行一次：
 * 1. 检查主库健康状态
 * 2. 检查所有从库健康状态
 * 3. 刷新所有从库的复制延迟
 * 4. 清理主库池中的失效连接
 * 5. 睡眠等待下一次检查
 */
void MySQLConnectionPool::HealthCheckLoop(int interval_seconds) {
    while (health_check_running_.load()) {
        // ========== 检查主库 ==========
        bool master_ok = CheckMasterHealth();
        if (!master_ok) {
            std::cerr << "[MySQL-ALERT] Master health check failed!" << std::endl;
            if (health_alert_callback_) {
                std::string node = host_ + ":" + std::to_string(port_);
                health_alert_callback_(node, false);
            }
        }

        // ========== 检查所有从库 ==========
        for (size_t i = 0; i < slave_pools_.size(); ++i) {
            bool slave_ok = CheckSlaveHealth(static_cast<int>(i));
            MetricsCollector::Instance().SetMySQLSlaveHealthy(static_cast<int>(i), slave_ok);

            if (!slave_ok && health_alert_callback_) {
                SlavePool& pool = *slave_pools_[i];
                std::string node = pool.host + ":" + std::to_string(pool.port);
                health_alert_callback_(node, false);
            }
        }

        // ========== 刷新复制延迟 ==========
        RefreshReplicationLag();

        // ========== 清理失效连接 ==========
        CleanExpiredConnections();

        // ========== 睡眠等待 ==========
        for (int i = 0; i < interval_seconds && health_check_running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    std::cout << "[MySQL] 健康检查已停止" << std::endl;
}

} // namespace reactor

