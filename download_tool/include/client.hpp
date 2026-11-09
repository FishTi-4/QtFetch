#pragma once

#include <mutex>
#include <condition_variable>
#include <atomic>

#include <string>
#include <utility>
#include <chrono>

#include "download_class.hpp"
#include "timer_wheel.hpp"
#include "thread_pool.hpp"

namespace download{

  extern Timer_Wheel timer;

  std::string read_all(int);

  class Worker{
  public:
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    Worker(std::string url, std::string save_path, int num_thread);

    ~Worker();

    int run();

    void pause();

    void resume();

    void stop();

    int is_done();

    double get_progress();

    double get_speed();

  private:
    int file_fd;
    download_class downloader;

    int have_download = 0;

    std::mutex mtx;
    std::condition_variable cv;

    std::string host;
    std::string filename;

    std::atomic<int> downloaded_size = 0;
    int last_download_size = 0;
    int need_download_size = -1;

    bool exit_flag = false;
    bool done_flag = false;

    std::atomic<double> speed = 0;

    int num_thread;
    std::pair<std::string, std::string> parse_url(std::string url);

    Worker(std::pair<std::string, std::string> parse_res, std::string save_path, int num_thread);

    void update_speed_loop();

    thread_pool work_threads;
  };
//==========================class===========================================================

} // namespace download
