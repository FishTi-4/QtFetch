#pragma once

#include "platform.hpp"
#include <string>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <condition_variable>

//download(string filename, string addr)
namespace download { 

/*
  这是前向声明，具体实现看后文
*/
int find_server_socket(std::string addr, std::string port = "80");
int send_message(int server_socket, std::string mes);

class download_class{
public:

  download_class& operator=(const download_class&) = delete;

  download_class(std::string filename, std::string host, std::mutex &mtx, std::condition_variable &cv, int file_fd, std::atomic<int> &download_history);

  ~download_class();

  int run(int start_byte, int end_byte);
  
  void pause();
  
  void resume();

  int is_done();

  void exit();
  

private:

  std::mutex& mtx;
  std::condition_variable& cv;

  std::atomic<int> pause_signal = 1;
  std::atomic<int> done_signal = 0;

  int server_socket = 0;
  int file_fd = 0;

  std::string file_name;
  std::string host;

  std::atomic<int>& download_history;

  int close();

  /*
    功能：
      工具函数，用于发出分块请求，

    参数：
      start_byte: 起始字节
      end_byte: 结束字节
      fd: 目标服务器的socket
  */
  int send_request(int start_byte, int end_byte, int fd);

  /*
    功能：
      发起连接（根据worker内部保留的链接）
    参数：
      无
  */
  int connect();

};

} // namespace download
