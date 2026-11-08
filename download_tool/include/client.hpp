#pragma once

#include "platform.hpp"

#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>

#include <string>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <cstdint>
#include <climits>
#include <memory>   // std::unique_ptr / std::make_unique：重定向后按最终地址重建 downloader
#include <utility>  // std::pair：parse_url 的返回值
#include <cctype>   // std::tolower：响应头名大小写不敏感比较
#include <cerrno>   // errno / EINTR：read_all 中被信号打断时重试

#include "download_class.hpp"
#include "http_parser.hpp"
#include "timer_wheel.hpp"

#define DEBUG

namespace download{

extern Timer_Wheel timer;

std::string read_all(int);

class Worker{
public:
Worker(const Worker&) = delete;
Worker& operator=(const Worker&) = delete;

Worker(std::string url, std::string save_path, int num_thread) :
Worker(parse_url(url), std::move(save_path), num_thread)
{
}

~Worker() {
if(downloader) downloader->exit();
exit_flag = true;
}

int run(){
try{
int max_redirects = 5; // [重定向处理] 最大跟随次数，防止死循环
int redirect_count = 0;

// [重定向处理] 使用 while 循环处理可能的多次跳转 (比如 A 跳 B，B 再跳 C)
while(true) {
int tp_fd = find_server_socket(host, "80");
if (tp_fd < 0) throw std::runtime_error("网络错误: 无法连接到服务器");

// 简单拼接请求路径
std::string req_path = (filename.empty() || filename[0] != '/') ? "/" + filename : filename;

send_message(tp_fd,
"HEAD " + req_path + " HTTP/1.1\r\n" +
"Host: " + host + "\r\n" +
"Connection: close\r\n" +
"\r\n"
);

std::string head = read_all(tp_fd);
close_socket(tp_fd);

// 空响应通常意味着连接被直接断开，parse_http 会读出脏数据，提前拦掉
if (head.empty()) throw std::runtime_error("服务器无响应");

HttpResponse res = parse_http(head);

if (res.version.empty()) {
throw std::runtime_error("协议错误: HTTP 响应头解析失败");
}

// [重定向处理] 核心逻辑：拦截 301 / 302 / 303 / 307 / 308
if (res.status_code == 301 || res.status_code == 302 ||
res.status_code == 303 || res.status_code == 307 || res.status_code == 308) {
if (++redirect_count > max_redirects) {
throw std::runtime_error("重定向次数过多，可能存在死循环");
}

// 寻找 Location 头 (忽略大小写)
std::string new_location = "";
for (auto const& [key, val] : res.headers) {
std::string lower_key = key;
std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
[](unsigned char c){ return (char)std::tolower(c); });
if (lower_key == "location") {
new_location = val;
break;
}
}

if (new_location.empty()) {
throw std::runtime_error("重定向错误: 服务器没有返回 Location 头");
}

// https 目标无法用当前明文 socket 处理，明确报错，避免把错误内容写进文件
std::string lower_loc = new_location;
std::transform(lower_loc.begin(), lower_loc.end(), lower_loc.begin(),
[](unsigned char c){ return (char)std::tolower(c); });
if (lower_loc.rfind("https://", 0) == 0) {
throw std::runtime_error("暂不支持重定向到 HTTPS: " + new_location);
}

// 只识别两种形式：
//   1. 绝对路径 /file.zip            -> 沿用当前 host
//   2. 完整 URL http://xxx/file.zip  -> 解析出新的 host
if (new_location[0] == '/') {
filename = new_location.substr(1);
} else if (lower_loc.rfind("http://", 0) == 0) {
auto parsed = parse_url(new_location);
if (parsed.first.empty()) {
throw std::runtime_error("重定向地址无效: " + new_location);
}
host = parsed.first;
filename = parsed.second;
} else {
throw std::runtime_error("无法识别的重定向地址: " + new_location);
}

std::cout << "遇到重定向 " << res.status_code << "，正在跳转到: " << new_location << std::endl;
continue; // 跳出本次循环，用新的 host 和 filename 重新发请求
}

// 处理 404
if (res.status_code == 404) {
throw std::runtime_error("404 Not Found: 网址不存在");
}

// 处理其他非 200 错误
if (res.status_code != 200) {
throw std::runtime_error("服务器返回错误状态码: " + std::to_string(res.status_code));
}

// 走到这里说明是 200 OK，成功拿到最终文件的真实响应，准备分片
// 头名大小写不敏感（服务器可能返回 accept-ranges / content-length 等写法）
auto find_header = [&](const char* name) -> std::string {
std::string lower_name = name;
std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
[](unsigned char c){ return (char)std::tolower(c); });
for (auto const& [key, val] : res.headers) {
std::string lower_key = key;
std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
[](unsigned char c){ return (char)std::tolower(c); });
if (lower_key == lower_name) return val;
}
return "";
};

std::string accept_ranges = find_header("Accept-Ranges");
if (accept_ranges != "bytes") {
std::cout << "服务器不支持断点续传，自动降级为单线程" << std::endl;
num_thread = 1;
}

std::string cl = find_header("Content-Length");
if (cl.empty()) {
throw std::runtime_error("数据错误: 服务器未返回 Content-Length");
}

// 使用 stoull 防范超大文件（负数/非法值会抛异常，被 catch 兜住）
uint64_t content_size = std::stoull(cl);
if(content_size == 0){
throw std::runtime_error("文件错误: 文件内容为空");
}

// 简单拦截超大型文件防溢出
if (content_size > INT_MAX) {
throw std::runtime_error("暂不支持超过 2GB 的文件");
}
need_download_size = (int)content_size;

#ifdef DEBUG
std::cout << "最终文件大小: " << content_size << " 字节" << std::endl;
#endif

// [注意] 因为 host 和 filename 可能变了，所以下载器必须在这里才初始化
downloader = std::make_unique<download_class>(filename, host, mtx, cv, file_fd, downloaded_size);

// 先从原子变量取一次快照，避免循环里反复读原子量（值本就不会再变）
int total_size = need_download_size;
int block_size = (total_size + num_thread - 1) / num_thread;

for(int i = 0; i < num_thread; i++){
int start_byte = i * block_size;
int end_byte = (std::min)(start_byte + block_size, total_size) - 1;
if(start_byte > end_byte) break;   // 文件很小时可能出现空块，直接跳过

work_threads.submit_task(
[this, start_byte, end_byte]{
downloader->run(start_byte, end_byte);
}
);
}

auto tm = std::chrono::milliseconds(100);
std::this_thread::sleep_for(tm);

timer.add_always_timer(std::chrono::milliseconds(1000), [this]{ update_speed_loop(); } );

break; // 成功启动下载任务，跳出重定向 while 循环
} // end while(true)

}
catch(std::exception &e){
std::cerr << "下载任务异常: " << e.what() << std::endl;
return -1;
}

return 0;
}

void pause(){
if(downloader) downloader->pause();
}

void resume(){
if(downloader) downloader->resume();
}

void stop(){
if(downloader) downloader->exit();
}

int is_done(){
return done_flag;
}

double get_progress(){
if(need_download_size <= 0) return 0.0;
return (double)downloaded_size / need_download_size;
}

double get_speed(){
return speed;
}

private:
int file_fd;
std::unique_ptr<download_class> downloader; // 延迟初始化

std::mutex mtx;
std::condition_variable cv;

std::string host;
std::string filename;

std::atomic<int> downloaded_size{0};
int last_download_size = 0;
std::atomic<int> need_download_size{-1};  // download_class 要的是 atomic<int>&，且多线程读写

bool exit_flag = false;
bool done_flag = false;

std::atomic<double> speed = 0;

int num_thread;

// 解析 URL (处理 http://host/path 格式)
std::pair<std::string, std::string> parse_url(std::string url){
// 如果带有 http://，先去掉
if (url.find("://") != std::string::npos) {
url = url.substr(url.find("://") + 3);
}

// 找第一个斜杠，前面是 host，后面是 path
size_t slash = url.find('/');
if (slash == std::string::npos) {
return {url, ""}; // 只有域名没有路径
}
return {url.substr(0, slash), url.substr(slash + 1)};
}

Worker(std::pair<std::string, std::string> parse_res, std::string save_path, int num_thread)
: file_fd(open(save_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644)),
work_threads(num_thread), num_thread(num_thread)
{
if(file_fd < 0){
throw std::runtime_error("无法打开保存文件: " + save_path);
}
host = parse_res.first;
filename = parse_res.second;
}

void update_speed_loop(){
int cur_size = downloaded_size - last_download_size;
speed = (double)cur_size;
last_download_size = downloaded_size;

if(exit_flag || done_flag) return;
if( downloaded_size >= need_download_size && need_download_size > 0){
std::cout << "下载完成" << std::endl;
done_flag = true;
exit_flag = true;
}
}

thread_pool work_threads;

};
//==========================class===========================================================

std::string read_all(int fd){
std::string buf;
char tmp[1024];

while(true){
int ret = recv(fd, tmp, sizeof(tmp), 0);
if (ret > 0) {
buf.append(tmp, ret);
continue;
}
if (ret == 0) break;          // 对端正常关闭
if (errno == EINTR) continue; // 被信号打断，重试
break;                        // 其它错误
}
return buf;

}

int find_server_socket(std::string addr, std::string port = "80"){
int server_socket = socket(AF_INET, SOCK_STREAM, 0);
if (server_socket < 0){
std::cout << "Error opening socket" << std::endl;
return -1;
}
addrinfo hints {};
hints.ai_family = AF_INET;
hints.ai_socktype = SOCK_STREAM;

addrinfo *server_info;
int ret = getaddrinfo(addr.c_str(), port.c_str(), &hints, &server_info);
if (ret != 0 || server_info == nullptr){
std::cout << "Error getaddrinfo: " << gai_strerror(ret) << std::endl;
return -1;
}
if (connect(server_socket, server_info->ai_addr, server_info->ai_addrlen) < 0){
std::cout << "Error connecting" << std::endl;
freeaddrinfo(server_info);
return -1;
}

#ifdef DEBUG
char ip[INET_ADDRSTRLEN];
inet_ntop(AF_INET, &((sockaddr_in*)server_info->ai_addr)->sin_addr, ip, sizeof(ip));
std::cout << "将从地址 " << ip << " 处下载" << std::endl;
#endif

freeaddrinfo(server_info);
return server_socket;

}

} // namespace download
