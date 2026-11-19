#include "../include/client.hpp"
#include <iostream>

int main()
{

  download::Worker wk("www.xinhuanet.com", "deepseek.html", 2);

  try{
    wk.run();
  }
  catch(std::runtime_error& e){
    std::cout << e.what() << std::endl;
  }


  std::cout << "END" << std::endl;


}