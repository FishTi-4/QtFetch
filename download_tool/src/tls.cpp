#include "tls.hpp"
#include <stdexcept>     // 为了 std::runtime_error
#include <iostream>
#include "platform.hpp"  // 为了 close_socket
#include <openssl/ssl.h>
#include <openssl/err.h>
namespace download {

  /*
    负责人：
    功能：初始化 SSL_CTX 管理器
    参数：无
    返回值：无
  */
  SSL_CTX_manager::SSL_CTX_manager() {
  }

  /*
    负责人：
    功能：释放 SSL_CTX 资源
    参数：无
    返回值：无
  */
  SSL_CTX_manager::~SSL_CTX_manager() {
  }

  /*
    负责人：
    功能：获取 SSL_CTX 指针
    参数：无
    返回值：SSL_CTX* 上下文指针
  */
  SSL_CTX* SSL_CTX_manager::get_ctx() noexcept {
    return nullptr;
  }

  /*
    负责人：
    功能：创建 SSL_CTX
    参数：无
    返回值：SSL_CTX* 上下文指针
  */
  SSL_CTX* SSL_CTX_manager::create_ctx() {
    return nullptr;
  }

  /*
    负责人：
    功能：析构连接基类
    参数：无
    返回值：无
  */
  connection::~connection() = default;

  /*
    功能：根据主机名和端口创建并连接一个服务端socket
    参数：hostname 主机名、port 端口
    返回值：成功返回socket描述符，失败返回-1
  */
  int connection::find_server_socket(std::string hostname, std::string port){
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

  /*
    负责人：younger-ing
    功能：建立 TLS 连接
    参数：hostname 主机名、port 端口、ctx SSL 上下文、host 主机名
    返回值：无
  */
  tls_connection::tls_connection(const std::string& host, const std::string& port, SSL_CTX* ctx)
    : ssl(nullptr), socket_fd(-1), host_(host) {
      socket_fd = connection::find_server_socket(host_, port);
      if (socket_fd < 0) {
        throw std::runtime_error("TCP Connection Failed for TLS: " + host + ":" + port);
    }//建立TCP连接
    ssl = SSL_new(ctx);
    if (!ssl) {
      close_socket(socket_fd);
      throw std::runtime_error("Failed to create SSL object");
    }//创建SSL对象
    SSL_set_fd(ssl, socket_fd);
    SSL_set_tlsext_host_name(ssl, host_.c_str());//绑定socket
    int ret = SSL_connect(ssl);
    if (ret != 1) {
      ERR_print_errors_fp(stderr); // 打印握手失败原因
      SSL_free(ssl);
      close_socket(socket_fd);
      throw std::runtime_error("SSL/TLS handshake failed");
    }//执行TLS握手
  }

  /*
    负责人：younger-ing
    功能：释放 TLS 连接资源
    参数：无
    返回值：无
  */
  tls_connection::~tls_connection() {
    close_connection();
  }

  /*
    负责人：younger-ing
    功能：通过 TLS 发送数据
    参数：buf 数据缓冲区、n 数据长度
    返回值：发送字节数
  */
  int tls_connection::send(const char* buf, size_t n) {
    if(!ssl)
      return -1;
    return SSL_write(ssl, buf, static_cast<int>(n));
  }

  /*
    负责人：younger-ing
    功能：通过 TLS 接收数据
    参数：buf 数据缓冲区、n 缓冲区大小
    返回值：接收字节数
  */
  int tls_connection::recv(char* buf, size_t n) {
    if(!ssl)
      return 0;
    int ret = SSL_read(ssl, buf, static_cast<int>(n));
    return ret > 0 ? ret : 0;
  }

  /*
    负责人：younger-ing
    功能：关闭 TLS 连接
    参数：无
    返回值：无
  */
  void tls_connection::close_connection() {
    if(ssl) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
      ssl = nullptr;
    }//安全关闭SSL
    if (socket_fd >= 0) {
      close_socket(socket_fd);
      socket_fd = -1;
    }//关闭Socket
  }

  /*
    负责人：younger-ing
    功能：建立 TCP 连接
    参数：host主机名，port端口号
    返回值：无
  */
  tcp_connection::tcp_connection(const std::string& host, const std::string& port) {
    socket_fd = connection::find_server_socket(host, port);
    if(socket_fd < 0) {
      throw std::runtime_error("TCP connection failed to " + host + ":" + port);
    }
  }

  /*
    负责人：younger-ing
    功能：释放 TCP 连接资源
    参数：无
    返回值：无
  */
  tcp_connection::~tcp_connection() {
    close_connection();
  }

  /*
    负责人：younger-ing
    功能：通过 TCP 发送数据
    参数：buf 数据缓冲区、n 数据长度
    返回值：发送字节数
  */
  int tcp_connection::send(const char* buf, size_t n) {//只支持单个封装
      int ret=::send(socket_fd,buf,n,0);
      return ret;
  }

  /*
    负责人：youger-ing
    功能：通过 TCP 接收数据
    参数：buf 数据缓冲区、n 缓冲区大小
    返回值：接收字节数
  */
  int tcp_connection::recv(char* buf, size_t n) {
    int ret=::recv(socket_fd,buf,n,0);
    if(ret<=0){
      return 0;
    }
    return ret;
  }

  /*
    负责人：youger-ing
    功能：关闭 TCP 连接
    参数：无
    返回值：无
  */
  void tcp_connection::close_connection() {
    if(socket_fd>=0) {
      close_socket(socket_fd);
      socket_fd=-1;
    }
  }

} // namespace download
