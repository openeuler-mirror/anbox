/*
 * Copyright (C) 2017 Simon Fels <morphis@gravedo.de>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "anbox/audio/server.h"
#include "anbox/audio/sink.h"
#include "anbox/audio/source.h"
#include "anbox/network/published_socket_connector.h"
#include "anbox/network/delegate_connection_creator.h"
#include "anbox/network/local_socket_messenger.h"
#include "anbox/network/message_processor.h"
#include "anbox/common/type_traits.h"
#include "anbox/system_configuration.h"
#include "anbox/utils.h"
#include "anbox/logger.h"

#include<stdlib.h>
#include<stdio.h>
#include<alsa/asoundlib.h>
#include<unistd.h>
#include<thread>

using namespace std::placeholders;

namespace {
class AudioForwarder : public anbox::network::MessageProcessor {
 public:
  AudioForwarder(const std::shared_ptr<anbox::audio::Sink> &sink) :
    sink_(sink) {
  }

  bool process_data(const std::vector<std::uint8_t> &data) override {
    std::thread::id tid = std::this_thread::get_id();
    pid_t pid  = getpid();
    DEBUG("audio sink forwarder is called,pid = %d ,tid = %d ", pid, tid);
    sink_->write_data(data);
    return true;
  }

 private:
  std::shared_ptr<anbox::audio::Sink> sink_;
};

class AudioRecorderForwarder: public anbox::network::MessageProcessor {
public:
    AudioRecorderForwarder(const std::shared_ptr<anbox::audio::Source> &source):
      source_(source){
    }

    bool process_data (const std::vector<std::uint8_t> &data) override
    {
        std::thread::id tid = std::this_thread::get_id();
        pid_t pid  = getpid();
        DEBUG("audio recorder forwarder is called ,pid = %d ,tid = %d ", pid, tid);
        source_->read_data(data);
        return true;
    }

    ~AudioRecorderForwarder()
    {
        source_.reset();
    }
private:
    std::shared_ptr<anbox::audio::Source> source_;
 };
}

namespace anbox {
namespace audio {
Server::Server(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<platform::BasePlatform> &platform) :
  platform_(platform),
  socket_file_(utils::string_format("%s/anbox_audio", SystemConfiguration::instance().socket_dir())),
  connector_(std::make_shared<network::PublishedSocketConnector>(
             socket_file_, rt,
             std::make_shared<network::DelegateConnectionCreator<boost::asio::local::stream_protocol>>(std::bind(&Server::create_connection_for, this, _1)))),
  connections_(std::make_shared<network::Connections<network::SocketConnection>>()),
  next_id_(0) {

  // FIXME: currently creating the socket creates it with the rights of
  // the user we're running as. As this one is mapped into the container
  ::chmod(socket_file_.c_str(), 0770);
}

Server::~Server() {}

void Server::create_connection_for(std::shared_ptr<boost::asio::basic_stream_socket<boost::asio::local::stream_protocol>> const& socket) {
  auto const messenger =
      std::make_shared<network::LocalSocketMessenger>(socket);

  // We have to read the client flags first before we can continue
  // processing the actual commands
  ClientInfo client_info;
  auto err = messenger->receive_msg(
      boost::asio::buffer(&client_info, sizeof(ClientInfo)));
  if (err) {
    ERROR("Failed to read client info: %s", err.message());
    return;
  }

  std::shared_ptr<network::MessageProcessor> processor;
  std::shared_ptr<anbox::audio::Source>  mAudioSource;

  switch (client_info.type) {
  case ClientInfo::Type::Playback:
    processor = std::make_shared<AudioForwarder>(platform_->create_audio_sink());
    DEBUG("create_connection for is called, audio type Playback");
    break;
  case ClientInfo::Type::Recording:
    DEBUG("create_connection for is called, audio type Recording");
    mAudioSource = platform_->create_audio_source();
    processor = std::make_shared< AudioRecorderForwarder>(mAudioSource);
    break;
  default:
    ERROR("Invalid client type %d", static_cast<int>(client_info.type));
    return;
  }

  // Everything ok, so approve the client by sending the requesting client
  // info back. Once we have more things to negotiate we will send a modified
  // client info struct back.
  messenger->send(reinterpret_cast<char*>(&client_info), sizeof(client_info));

  auto connection = std::make_shared<network::SocketConnection>(
        messenger, messenger, next_id(), connections_, processor);
  if (client_info.type == ClientInfo::Type::Recording) {
    mAudioSource->set_socket_connection(connection);
    mAudioSource->connect_audio();
  }
  connections_->add(connection);
  connection->read_next_message();
}

int Server::next_id() {
  return next_id_.fetch_add(1);
}
} // namespace audio
} // namespace anbox
