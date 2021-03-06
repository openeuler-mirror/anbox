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

#include "anbox/platform/sdl/audio_sink.h"
#include "anbox/logger.h"

#include <stdexcept>

#include <boost/throw_exception.hpp>

namespace {
const constexpr size_t max_queue_size{16};
}

namespace anbox {
namespace platform {
namespace sdl {
AudioSink::AudioSink() :
  device_id_(0),
  queue_(max_queue_size),
  mThreadExit(false),
  t(NULL) {
}

AudioSink::~AudioSink() {
  mThreadExit = true;
  if (t != NULL) {
    free(t);
    t = NULL;
  }
  queue_.close_locked();
  disconnect_audio();
}

void AudioSink::on_data_requested(void *user_data, std::uint8_t *buffer, int size) {
  auto thiz = static_cast<AudioSink*>(user_data);
  thiz->read_data(buffer, size);
}

void AudioSink::monitor_loop()
{
    while (!mThreadExit) {
      if (device_id_ > 0 && SDL_GetAudioDeviceStatus(device_id_) == SDL_AUDIO_STOPPED) {
        // restart audio if stopped internal cause of alsa errors
        ERROR("closing sdl audio device cause of error device_id_ %d", device_id_);
        SDL_LockAudioDevice(device_id_);
        SDL_CloseAudioDevice(device_id_);
        SDL_UnlockAudioDevice(device_id_);
        ERROR("closed sdl audio device cause of error device_id_ %d", device_id_);
        device_id_ = 0;
      }
      usleep(1*1000*1000);
    }
}

bool AudioSink::connect_audio() {
  if (device_id_ > 0) {
    return true;
  }

  SDL_memset(&spec_, 0, sizeof(spec_));
  spec_.freq = 44100;
  spec_.format = AUDIO_S16;
  spec_.channels = 2;
  spec_.samples = 1024;
  spec_.callback = &AudioSink::on_data_requested;
  spec_.userdata = this;

  device_id_ = SDL_OpenAudioDevice(nullptr, 0, &spec_, nullptr, 0);
  if (!device_id_)
    return false;

  SDL_PauseAudioDevice(device_id_, 0);

  if(t == NULL){
    t = new std::thread([this](){
      this->monitor_loop();
    });
    t->detach();
  }
  return true;
}

void AudioSink::disconnect_audio() {
  if (device_id_ == 0)
    return;

  SDL_CloseAudioDevice(device_id_);
  device_id_ = 0;
}

void AudioSink::read_data(std::uint8_t *buffer, int size) {
  std::unique_lock<std::mutex> l(lock_);
  const auto wanted = size;
  int count = 0;
  auto dst = buffer;

  while (count < wanted) {
    if (read_buffer_left_ > 0) {
      size_t avail = std::min<size_t>(wanted - count, read_buffer_left_);
      memcpy(dst + count,
             read_buffer_.data() + (read_buffer_.size() - read_buffer_left_),
             avail);
      count += avail;
      read_buffer_left_ -= avail;
      continue;
    }

    bool blocking = (count == 0);
    auto result = -EIO;
    if (blocking)
      result = queue_.pop_locked(&read_buffer_, l);
    else
      result = queue_.try_pop_locked(&read_buffer_);

    if (result == 0) {
      read_buffer_left_ = read_buffer_.size();
      continue;
    }

    if (count > 0) break;

    return;
  }
}

void AudioSink::write_data(const std::vector<std::uint8_t> &data) {
  std::unique_lock<std::mutex> l(lock_);
  if(data.size() == 0) {
    WARNING("AudioSink::write_data with 0 size");
    return;
  }
  if (!connect_audio()) {
    ERROR("Audio server not connected, skipping %d bytes", data.size());
    return;
  }
  graphics::Buffer buffer{data.data(), data.data() + data.size()};
  if(!queue_.can_push_locked()){
    ERROR("AudioSink buffer queue full, skipping %d bytes", data.size());
    return;
  }
  queue_.push_locked(std::move(buffer), l);
}
} // namespace sdl
} // namespace platform
} // namespace anbox
