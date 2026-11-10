#include "client.hpp"
#include "http_parser.hpp"

#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace download{

  std::string read_all(int fd){ 
    
    std::string buf;
    char tmp[1024];

    while(true){ 
      int ret = recv(fd, tmp, 1024, 0);
      if (ret <= 0) break;
      buf += std::string(tmp, ret);
    }

    return std::move(buf);
  }

  int find_server_socket(std::string hostname, std::string port){
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0){
      std::cout << "Error opening socket" << std::endl;
      return -1;
    }
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
  
    addrinfo *server_info;
    int ret = getaddrinfo(hostname.c_str(), port.c_str(), &hints, &server_info);
    if (ret != 0 || server_info == nullptr){
      std::cout << "Error getaddrinfo: " << gai_strerror(ret) << std::endl;
      return -1;
    }
    if (connect(server_socket, server_info->ai_addr, server_info->ai_addrlen) < 0){
      std::cout << "Error connecting" << std::endl;
      freeaddrinfo(server_info);
      return -1;
    }

    freeaddrinfo(server_info);
    return server_socket;
  }

  Worker::Worker(std::string url, std::string save_path, int num_thread) : 
    Worker(parse_url(url), std::move(save_path), num_thread)
  {
  }

  Worker::Worker(std::tuple<std::string, std::string, std::string> parsed_url, std::string save_path, int num_thread)
    : file_fd(open(save_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644)),
      protocol(std::get<0>(parsed_url)),
      host(std::get<1>(parsed_url)),
      path(std::get<2>(parsed_url)),
      work_threads(num_thread), num_thread(num_thread)
  {
    if(file_fd < 0){
      throw std::runtime_error("无法打开保存文件: " + save_path);
    }
  }

  Worker::~Worker() {
    if (downloader) downloader->exit();
    exit_flag = true;
  }

  int Worker::run(){

    try{

      std::string port = (protocol == "https") ? "443" : "80";
      int server_fd = find_server_socket(host, port);

      send_message(server_fd, 
        "HEAD /" + path + " HTTP/1.1\r\n" +
        "Host: " + host + "\r\n" +
        "Connection: close\r\n" +
        "\r\n"
      );

      std::string head = read_all(server_fd);
      HttpParser res(head);

      /*
        3XX错误， 更新 protocol, host, path
      */
      if(res.status_code() / 100 == 3 ){
        close_socket(server_fd);
        std::string redirect_url = Redirect(res);
        auto redirect_addr = parse_url(redirect_url);
        protocol = std::get<0>(redirect_addr);
        host = std::get<1>(redirect_addr);
        path = std::get<2>(redirect_addr);
        port = (protocol == "https") ? "443" : "80";
      }

      // 404：网址不存在
      if(res.status_code() == 404){
        close_socket(server_fd);
        throw std::runtime_error("404 Not Found，网址不存在");
      }

      // 其他非 200 状态码
      if(res.status_code() != 200){
        close_socket(server_fd);
        throw std::runtime_error("服务器返回错误状态码: " + std::to_string(res.status_code()));
      }

      if(res.get("Accept-Ranges") == "bytes" ){}
      else{
        std::cout << "服务器不支持断点续传" << std::endl;
        num_thread = 1;
      } 
      close_socket(server_fd);

      timer.add_always_timer(std::chrono::milliseconds(1000), [this](){ update_speed_loop(); } );

      int content_size = stoi(res.get("Content-Length"));
      if(content_size == 0){
        throw std::runtime_error("文件为空");
      }
      need_download_size = content_size;
      
      downloader = std::make_unique<download_class>(path, host, port, mtx, cv, file_fd, downloaded_size);

      int block_size = (content_size + num_thread - 1) / num_thread;

      for(int i = 0; i < num_thread; i++){
        int start_byte = i * block_size;
          int end_byte = (std::min)(start_byte + block_size, content_size) - 1;
        work_threads.submit_task(
          [this, start_byte, end_byte](){
            downloader->run(start_byte, end_byte);
          }
        );
      }

    }
    catch(std::exception &e){
      std::cout << e.what() << std::endl;
    }

    return 0;
  }

  void Worker::pause(){
    if (downloader) downloader->pause();
  }

  void Worker::resume(){
    if (downloader) downloader->resume();
  }

  void Worker::stop(){
    if (downloader) downloader->exit();
  }

  int Worker::is_done(){
    return done_flag;
  }

  double Worker::get_progress(){
    return (double)downloaded_size / need_download_size;
  }

  double Worker::get_speed(){
    return speed;
  }

  /*
    功能：解析 url，变为 (protocol, host, path)
    参数：url - 待解析的 url
    返回：(protocol, host, path)
  */
  std::tuple<std::string ,std::string, std::string> Worker::parse_url(std::string url){
    int pos = url.find("://");
    std::string protocol;
    if(pos == std::string::npos){
      protocol = "https";
    }

    if(pos != std::string::npos){
      protocol = url.substr(0, pos);
      if(protocol != "http" && protocol != "https")
        protocol = "https";
    }

    if (pos == std::string::npos) pos = 0;
    else  pos += 3;

    int cut = url.find("/", pos);

    if(cut == std::string::npos) return {protocol, url.substr(pos), ""};
    else return {protocol, url.substr(pos, cut - pos), url.substr(cut + 1)};
  }

  void Worker::update_speed_loop(){ 
    while(1){
    
      int cur_size = downloaded_size - last_download_size;
      
      speed = cur_size;
      last_download_size = downloaded_size;
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));

      if(exit_flag || done_flag) break;
      if( downloaded_size == need_download_size ){
        std::cout << "下载完成" << std::endl;
        exit_flag = true;
      }
    }
  }

  std::string Worker::Redirect(HttpParser& response){
    return "";
  }


} // namespace download
