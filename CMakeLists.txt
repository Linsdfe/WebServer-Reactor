# ==============================================================================
# CMakeLists.txt：项目构建配置
# 核心功能：
# 1. 设置C++标准、编译选项
# 2. 收集源文件、生成可执行文件
# 3. 链接系统库（pthread）
# 4. 复制静态资源（www目录）到输出目录
# ==============================================================================
cmake_minimum_required(VERSION 3.10)
# 项目信息：名称WebServer-Reactor，版本1.0，语言C++
project(WebServer-Reactor VERSION 1.0 LANGUAGES CXX)

# ========== C++标准配置 ==========
# 强制C++11标准，关闭编译器扩展（保证跨平台兼容性）
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ========== 编译选项 ==========
# GCC/Clang编译器：开启全量警告+O2优化（Release）/调试信息（Debug）
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # 警告选项：Wall（基础警告）+ Wextra（额外警告）+ Wpedantic（严格语法）
    add_compile_options(-Wall -Wextra -Wpedantic)
    # Release模式：O2优化（提升运行效率）
    set(CMAKE_CXX_FLAGS_RELEASE "-O2")
    # Debug模式：-g（调试信息）+ O0（关闭优化）+ DEBUG宏
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
endif()

# ========== 头文件路径 ==========
# 全局头文件搜索路径：include目录（避免#include "../../"）
include_directories(${PROJECT_SOURCE_DIR}/include)

# ========== 源文件收集 ==========
# 递归收集所有源文件（net/http/server/main）
file(GLOB_RECURSE SOURCE_FILES
    "src/net/*.cpp"
    "src/http/*.cpp"
    "src/server/*.cpp"
    "src/main.cpp"
    "src/auth/*.cpp"
    
)

# ========== 可执行文件 ==========
# 生成可执行文件：server（输出到bin目录）
add_executable(server ${SOURCE_FILES})

# ========== 链接库 ==========
# 链接pthread线程库（C++11 thread依赖）
target_link_libraries(server PRIVATE pthread)
# 链接MySQL库
target_link_libraries(server PRIVATE mysqlclient)
# 链接OpenSSL库
target_link_libraries(server PRIVATE ssl crypto)

# ========== 输出目录 ==========
# 设置可执行文件输出目录：build/bin
set_target_properties(server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
)

# ========== 静态资源复制 ==========
# 编译后自动复制www目录到bin目录（保证静态资源路径正确）
add_custom_command(TARGET server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${PROJECT_SOURCE_DIR}/www
    $<TARGET_FILE_DIR:server>/www
    COMMENT "Copy static resource directory www to bin/"
)

# ========== 单元测试（可选） ==========
# 后续扩展：开启单元测试时编译test目录
# option(BUILD_TEST "Build unit test" OFF)
# if(BUILD_TEST)
#     enable_testing()
#     add_subdirectory(test)
# endif()