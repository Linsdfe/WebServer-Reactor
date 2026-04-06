cmake_minimum_required(VERSION 3.10)
project(WebServer-Reactor VERSION 1.0 LANGUAGES CXX)

# 强制C++11标准，关闭编译器扩展
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项：开启全量警告+O2优化，Debug模式加-g调试信息
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
endif()

# 头文件全局搜索路径
include_directories(${PROJECT_SOURCE_DIR}/include)

# 递归收集所有源文件
file(GLOB_RECURSE SOURCE_FILES
    "src/net/*.cpp"
    "src/http/*.cpp"
    "src/server/*.cpp"
    "src/main.cpp"
)

# 生成可执行文件
add_executable(server ${SOURCE_FILES})

# 链接Linux系统库（pthread线程库）
target_link_libraries(server PRIVATE pthread)

# 设置可执行文件输出目录
set_target_properties(server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
)

# 编译后自动复制静态资源目录到可执行文件同级
add_custom_command(TARGET server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${PROJECT_SOURCE_DIR}/www
    $<TARGET_FILE_DIR:server>/www
    COMMENT "Copy static resource directory www to bin/"
)

# 可选：开启单元测试（后续扩展）
# option(BUILD_TEST "Build unit test" OFF)
# if(BUILD_TEST)
#     enable_testing()
#     add_subdirectory(test)
# endif()
