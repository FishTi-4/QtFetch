#include "download_class.hpp"

#include <stdexcept>

namespace download {

  /*
    功能：
      通用全局工具函数，用于发送消息，封装消息一次性发不完整的情况

    参数：
      server_socket: 目标服务器的socket
      mes: 要发送的消息
  */
  int send_message(int server_socket, std::string mes){
    size_t sent = 0;
    while (sent < mes.size()){
      int n = send(server_socket, mes.c_str() + sent, mes.size() - sent, 0);
      if (n < 0) return -1;
      sent += n;
    }
    return 0;
  }

  download_class::download_class(std::string filename, std::string host, std::mutex &mtx, std::condition_variable &cv, int file_fd, std::atomic<int> &download_history)
  : host(host), file_name(filename), mtx(mtx), cv(cv), file_fd(file_fd), download_history(download_history)
  {
  }

  download_class::~download_class(){
    if(server_socket){
      close_socket(server_socket);
    }
    if(file_fd){
      ::close(file_fd);
    }
  }

  int download_class::run(int start_byte, int end_byte){
      pause_signal = 0;
      int offset = start_byte;

      int server_fd = find_server_socket(host, "80");
      send_request(start_byte, end_byte, server_fd);

      std::string buf;
      char tmp[4096];
      bool header_done = false;

      while(1){

          {
              std::unique_lock<std::mutex> lk(mtx);
              cv.wait(lk, [&](){ return !pause_signal || done_signal; });
          }
          if(done_signal) break;

          int sz = recv(server_fd, tmp, sizeof(tmp), 0);
          if(sz < 0){
              close_socket(server_fd);
              throw std::runtime_error("cannot recv");
          }
          if(sz == 0) break;

          buf.append(tmp, sz);

          if(!header_done){
              int pos = buf.find("\r\n\r\n");
              if(pos == std::string::npos) continue;
              buf = buf.substr(pos + 4);
              header_done = true;
          }

          while(!buf.empty()){
              int n = pwrite_at(file_fd, buf.data(), buf.size(), offset);
              if(n <= 0){
                  close_socket(server_fd);
                  throw std::runtime_error("cannot write");
              }
              offset += n;
              download_history += n;
              buf.erase(0, n);
          }
      }

      close_socket(server_fd);
      return 0;
  }
  
  void download_class::pause(){
    pause_signal = 1;
  }
  
  void download_class::resume(){
    pause_signal = 0;
    cv.notify_all();
  }

  int download_class::is_done(){
    return done_signal;
  }

  void download_class::exit(){
    done_signal = 1;
    cv.notify_all();
  }
  

  int download_class::close(){
    if(server_socket){
      close_socket(server_socket);
      server_socket = 0;
    }
    return 0;
  }

  int download_class::send_request(int start_byte, int end_byte, int fd){       
    std::string mes = "GET /" + file_name + " HTTP/1.1\r\n" +
                      "Host: " + host + "\r\n" +
                      "Connection: close\r\n" +
                      "Range: bytes=" + std::to_string(start_byte) + "-" + std::to_string(end_byte) + "\r\n" +
                      "\r\n";
    send_message(fd, mes);
    return 0;
  }

  int download_class::connect(){
    server_socket = find_server_socket(host, "80");
    if(server_socket < 0){
      throw std::runtime_error("cannot connect to server");
    }
    return 0;
  }

} // namespace download
