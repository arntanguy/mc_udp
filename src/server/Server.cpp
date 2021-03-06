/*
 * Copyright 2019-2020 CNRS-UM LIRMM, CNRS-AIST JRL
 */

#include <mc_udp/data/Hello.h>
#include <mc_udp/data/Init.h>
#include <mc_udp/logging.h>
#include <mc_udp/server/Server.h>

#include <stdexcept>
#include <string.h>
#include <unistd.h>

namespace mc_udp
{

Server::Server() : socket_(0), recvData_(1024, 0), sendData_(1024, 0), initClient_(false), waitInit_(false) {}

Server::Server(int port) : recvData_(1024, 0), sendData_(1024, 0), initClient_(false), waitInit_(false)
{
  start(port);
}

Server::~Server()
{
  stop();
}

bool Server::recv()
{
  int length =
      recvfrom(socket_, recvData_.data(), recvData_.size(), MSG_DONTWAIT, (struct sockaddr *)&client_, &clientAddrLen_);
  if(length > 0)
  {
    if(length == sizeof(Hello) * sizeof(uint8_t))
    {
      MC_UDP_INFO(id_ << " New client sending data")
      initClient_ = true;
      waitInit_ = true;
    }
    else if(length == sizeof(Init) * sizeof(uint8_t))
    {
      MC_UDP_INFO(id_ << " Start streaming data to client")
      sensors().id = 0;
      initClient_ = false;
    }
    else if(length >= static_cast<int>(recvData_.size()))
    {
      MC_UDP_WARNING(id_ << " Received exactly the buffer size, resizing for next round")
      recvData_.resize(2 * recvData_.size());
    }
    else
    {
      control_.fromBuffer(recvData_.data());
      return true;
    }
  }
  return false;
}

void Server::send()
{
  size_t sz = sensors_.size();
  if(sz > sendData_.size())
  {
    MC_UDP_WARNING(id_ << " Send data buffer is too small for required sending (size: " << sendData_.size()
                       << ", required: " << sz << ")")
    sendData_.resize(sz);
  }
  sensors_.toBuffer(sendData_.data());
  if((initClient_ && waitInit_) || !initClient_)
  {
    waitInit_ = false;
    sendto(socket_, sendData_.data(), sz, 0, (struct sockaddr *)&client_, clientAddrLen_);
  }
}

void Server::stop()
{
  if(socket_ != 0)
  {
    close(socket_);
  }
}

void Server::restart(int port)
{
  stop();
  start(port);
}

void Server::start(int port)
{
  std::stringstream ss;
  ss << "[UDP::" << port << "]";
  id_ = ss.str();
  socket_ = socket(AF_INET, SOCK_DGRAM, 0);
  if(socket_ < 0)
  {
    MC_UDP_ERROR_AND_THROW(std::runtime_error, "Failed to create socket: " << strerror(errno))
  }
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  int err = bind(socket_, (struct sockaddr *)&addr, sizeof(addr));
  if(err < 0)
  {
    MC_UDP_ERROR_AND_THROW(std::runtime_error, "Failed bind the socket: " << strerror(errno))
  }
  clientAddrLen_ = sizeof(client_);
}

} // namespace mc_udp
