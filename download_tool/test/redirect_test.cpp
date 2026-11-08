// ===========================================================================
// client.hpp 重定向行为测试
// ---------------------------------------------------------------------------
// 被测场景：http://deepseek.com 返回 301，Location 指向 https://deepseek.com/
//
// 当前实现只支持明文 80 端口，遇到 https 目标会明确报错而不是继续硬连。
// 所以本测试验证的是“重定向链路本身工作正常”：
//   1. 能连上服务器、发出 HEAD、拿到 301 响应
//   2. 能正确解析出 Location 头
//   3. 识别出目标是 https 并给出“暂不支持重定向到 HTTPS”的错误
//   4. 不会陷入死循环，也不会把错误页写进文件
//
// 由于 Worker::run() 把异常原因只打印到 std::cerr、对外只返回 -1，
// 这里用一个临时重定向把 cerr 抓下来做断言。
// ===========================================================================

#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>

#include "../include/client.hpp"

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// 迷你测试框架：与 client_test.cpp / timer_wheel_test.cpp 保持一致
// ---------------------------------------------------------------------------

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const std::string& msg) {
  g_checks++;
  if (ok) {
    std::cout << "    [PASS] " << msg << std::endl;
  } else {
    std::cout << "    [FAIL] " << msg << std::endl;
    g_failed++;
  }
}

// 把 std::cerr 的这段输出临时收集到字符串里，方便断言错误原因
class CerrCapture {
public:
  CerrCapture() : old_buf(std::cerr.rdbuf(buffer.rdbuf())) {}
  ~CerrCapture() { std::cerr.rdbuf(old_buf); }

  std::string str() const { return buffer.str(); }

private:
  std::stringstream buffer;
  std::streambuf* old_buf;
};

static bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 用例 1：http://deepseek.com -> 301 -> https://deepseek.com/
//
// 期望：
//   · run() 返回 -1（因为最终目标 https 无法用明文 socket 下载）
//   · stderr 里出现“暂不支持重定向到 HTTPS”，且带上 deepseek.com
//   · 进度保持 0，没有被当成文件长度
//   · 保存文件被创建但内容为空（O_TRUNC 打开，未写入任何字节）
// ---------------------------------------------------------------------------
static void test_deepseek_301_to_https() {
  const std::string url = "http://deepseek.com";
  const std::string save_name = "test_deepseek_redirect.out";
  std::filesystem::remove(save_name);

  int ret = 0;
  std::string err;
  {
    download::Worker c(url, save_name, 2);
    CerrCapture cap;
    ret = c.run();
    err = cap.str();

    check(ret == -1, "run() 返回 -1（最终目标是 https，无法明文下载）");
    check(contains(err, "暂不支持重定向到 HTTPS"),
          "错误信息提示不支持重定向到 HTTPS");
    check(contains(err, "deepseek.com"),
          "错误信息包含目标域名 deepseek.com");
    check(!contains(err, "服务器返回错误状态码"),
          "没有被误判成普通状态码错误");
    check(!contains(err, "重定向次数过多"),
          "没有进入重定向死循环");

    check(c.get_progress() == 0.0, "进度保持 0，未把错误页当数据");
    check(!c.is_done(), "失败路径下 is_done() 仍为 false");
  }

  std::filesystem::path save = std::filesystem::absolute(save_name);
  if (std::filesystem::exists(save)) {
    check(std::filesystem::file_size(save) == 0, "保存文件为空，未写入错误页");
  } else {
    check(false, "保存文件应已被创建");
  }

  std::filesystem::remove(save);
}

// ---------------------------------------------------------------------------
// 用例 2：不带路径的 URL（http://deepseek.com/）同样应走 301 分支
//
// 覆盖点：parse_url 对“只有域名”的输入会得到空 path，
// run() 需要把它补成 "/" 才能发出合法的请求行。
// ---------------------------------------------------------------------------
static void test_deepseek_root_path() {
  const std::string url = "http://deepseek.com/";
  const std::string save_name = "test_deepseek_root.out";
  std::filesystem::remove(save_name);

  int ret = 0;
  std::string err;
  {
    download::Worker c(url, save_name, 1);
    CerrCapture cap;
    ret = c.run();
    err = cap.str();
  }

  check(ret == -1, "根路径 URL 同样返回 -1");
  check(contains(err, "暂不支持重定向到 HTTPS"),
        "根路径 URL 也正确识别 https 重定向");

  std::filesystem::remove(save_name);
}

// ---------------------------------------------------------------------------
// 用例 3：重定向拦截不应影响正常 200 站点
//
// 用已知可直连的 http 站点做对照，确认重定向逻辑没有把所有响应
// 都当成跳转处理（回归保护）。
// ---------------------------------------------------------------------------
static void test_normal_200_still_works() {
  const std::string url = "http://www.xinhuanet.com/";
  const std::string save_name = "test_redirect_control.out";
  std::filesystem::remove(save_name);

  download::Worker c(url, save_name, 2);

  int ret = 0;
  std::string err;
  {
    CerrCapture cap;
    ret = c.run();
    err = cap.str();
  }

  check(ret == 0, "普通 200 站点 run() 返回 0");
  check(!contains(err, "重定向"), "普通站点没有被误判为重定向");

  // run() 只是提交分块任务，给工作线程一点时间落盘后再清理
  std::this_thread::sleep_for(300ms);
  std::filesystem::remove(save_name);
}

// ---------------------------------------------------------------------------

static void run(const char* name, void (*fn)()) {
  std::cout << "[测试] " << name << std::endl;
  fn();
}

int main() {
  std::cout << "== redirect_test ==" << std::endl;

  run("1. http://deepseek.com 301 -> https://deepseek.com/", test_deepseek_301_to_https);
  run("2. 根路径 URL 的 301 处理", test_deepseek_root_path);
  run("3. 普通 200 站点不受影响", test_normal_200_still_works);

  std::cout << "----------------------------------------" << std::endl;
  std::cout << "共 " << g_checks << " 项断言, 失败 " << g_failed << " 项"
            << std::endl;
  if (g_failed > 0) {
    std::cout << "结果: FAIL" << std::endl;
    return 1;
  }
  std::cout << "结果: ALL PASS" << std::endl;
  return 0;
}
