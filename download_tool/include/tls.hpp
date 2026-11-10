#pragma once

#include <cstddef>
#include <openssl/ossl_typ.h>

namespace download {

class SSL_CTX_manager {
public:
  SSL_CTX_manager();
  ~SSL_CTX_manager(); 
  SSL_CTX* get_ctx() noexcept;
  SSL_CTX_manager(const SSL_CTX_manager&) = delete;
  SSL_CTX_manager& operator=(const SSL_CTX_manager&) = delete;

private:
  SSL_CTX* create_ctx();

  SSL_CTX* ctx;
};



class connection {
public:
  virtual ~connection() = 0;
  virtual int  send(const char* buf, size_t n) = 0;
  virtual int  recv(char* buf, size_t n)       = 0;
  virtual void close_connection()              = 0;

  connection(const connection&) = delete;
  connection& operator=(const connection&) = delete;

protected:
  connection() = default;
};

class tls_connection : public connection {
public:

  tls_connection(int socket_fd, SSL_CTX* ctx, const char* host);
  ~tls_connection() override;

  int  send(const char* buf, size_t n) override;
  int  recv(char* buf, size_t n)       override;
  void close_connection()              override;

private:
  SSL* ssl;
  int  socket_fd;
};



class tcp_connection : public connection {
public:
  explicit tcp_connection(int socket_fd);
  ~tcp_connection() override;

  int  send(const char* buf, size_t n) override;
  int  recv(char* buf, size_t n)       override;
  void close_connection()              override;

private:
  int socket_fd;
};


} // namespace download
