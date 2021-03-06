/*
 * Copyright (C) 2016 Simon Fels <morphis@gravedo.de>
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

#include "anbox/application/database.h"
#include "anbox/wm/multi_window_manager.h"
#include "anbox/platform/base_platform.h"
#include "anbox/bridge/android_api_stub.h"
#include "anbox/logger.h"
#include <SDL2/SDL.h>

#include <algorithm>

namespace anbox {
namespace wm {
MultiWindowManager::MultiWindowManager(const std::weak_ptr<platform::BasePlatform> &platform,
                                       const std::shared_ptr<bridge::AndroidApiStub> &android_api_stub,
                                       const std::shared_ptr<application::Database> &app_db)
    : platform_(platform), android_api_stub_(android_api_stub), app_db_(app_db) {}

MultiWindowManager::~MultiWindowManager() {}

void MultiWindowManager::apply_window_state_update(const WindowState::List &updated,
                                        const WindowState::List &removed) {
  // Base on the update we get from the Android WindowManagerService we will
  // create different window instances with the properties supplied. Incoming
  // layer updates from SurfaceFlinger will be mapped later into those windows
  // and eventually composited there via GLES (e.g. for popups, ..)

  std::map<Task::Id, WindowState::List> task_updates;

  bool neet_setfocus = false;
  bool is_window_removed = false;
  WindowState last_ws;
  for (const auto &window : updated) {
    // Ignore all windows which are not part of the freeform task stack
    if (window.stack() != Stack::Id::Freeform && window.stack() != Stack::Id::Fullscreen) continue;

    // And also those which don't have a surface mapped at the moment
    if (!window.has_surface()) continue;

    neet_setfocus = true;
    last_ws = window;
    // If we know that task already we first collect all window updates
    // for it so we can apply all of them together.
    auto t = task_updates.find(window.task());
    if (t == task_updates.end())
      task_updates.insert({window.task(), {window}});
    else
      task_updates[window.task()].push_back(window);
    if (find_window_for_task(window.task()) != nullptr) {
      continue;
    }

    if (window.frame().width() == 0 || window.frame().height() == 0) {
      continue;
    }

    auto title = window.package_name();
    auto app = app_db_->find_by_package(window.package_name());
    if (app.valid()) {
      title = app.name;
    }
    if (auto p = platform_.lock()) {
      SDL_Event event;
      SDL_memset(&event, 0, sizeof(event));
      event.type = p->get_user_window_event();
      event.user.code = platform::USER_CREATE_WINDOW;
      event.user.data1 = new(std::nothrow) platform::manager_window_param(window.task(), window.frame(), title);
      event.user.data2 = 0;
      SDL_PushEvent(&event);
      neet_setfocus = false;
    }
  }

  {
    // As final step we process all windows we need to remove as they
    // got killed on the other side. We need to respect here that we
    // also get removals for windows which are part of a task which is
    // still in use by other windows.
    std::lock_guard<std::mutex> l(mutex_);
    for (auto it = windows_.begin(); it != windows_.end(); ++it) {
      auto w = task_updates.find(it->first);
      if (w != task_updates.end()) {
        it->second->update_state(w->second);
        continue;
      }
      auto platform_window = it->second;
      platform_window->release();

      if (auto p = platform_.lock()) {
        SDL_Event event;
        SDL_memset(&event, 0, sizeof(event));
        event.type = p->get_user_window_event();
        event.user.code = platform::USER_DESTROY_WINDOW;
        event.user.data1 = new(std::nothrow) platform::manager_window_param(it->first, graphics::Rect(0, 0, 0, 0), "");
        event.user.data2 = 0;
        SDL_PushEvent(&event);
        is_window_removed = true;
      }
    }
  }
  if (neet_setfocus) {
    auto w = find_window_for_task(last_ws.task());
    if (w) {
      w->set_focus_from_android(is_window_removed);
    }
  }
}

std::shared_ptr<Window> MultiWindowManager::find_window_for_task(const Task::Id &task) {
  std::lock_guard<std::mutex> l(mutex_);
  for (const auto &w : windows_) {
    if (w.second->task() == task) return w.second;
  }
  return nullptr;
}

void MultiWindowManager::resize_task(const Task::Id &task, const anbox::graphics::Rect &rect,
                                      const std::int32_t &resize_mode) {
  android_api_stub_->resize_task(task, rect, resize_mode);
}

void MultiWindowManager::set_focused_task(const Task::Id &task) {
  android_api_stub_->set_focused_task(task);
}

void MultiWindowManager::remove_task(const Task::Id &task) {
  android_api_stub_->remove_task(task);
}

void MultiWindowManager::insert_task(const Task::Id &task, std::shared_ptr<wm::Window> pt) {
  std::lock_guard<std::mutex> l(mutex_);
  windows_.insert({ task, pt });
}

void MultiWindowManager::erase_task(const Task::Id &task) {
  std::lock_guard<std::mutex> l(mutex_);
  auto it = windows_.find(task);
  if (it != windows_.end()) {
    windows_.erase(it);
  }
}

std::string MultiWindowManager::get_title(const std::string &package_name) {
  auto app = app_db_->find_by_package(package_name);
  if (app.valid()) {
    return app.name;
  } else {
    return package_name;
  }
}

std::shared_ptr<Window> MultiWindowManager::get_toast_window(const anbox::graphics::Rect &rect, int index) {
  auto update_window = wm::WindowState{
      wm::Display::Id{0},
      true,
      rect,
      "",
      wm::Task::Id{0},
      wm::Stack::Id::Freeform,
  };
  auto tw = toast_windows_[index];
  if (tw != nullptr) {
    tw->update_state({update_window});
  }
  return tw;
}

void MultiWindowManager::add_toast_window(std::shared_ptr<Window> tw) {
  toast_windows_.push_back(tw);
}

void MultiWindowManager::hide_rest_toast_window(int index) {
  if (index < 0) return;

  auto update_window = wm::WindowState{
      wm::Display::Id{0},
      true,
      anbox::graphics::Rect{0, 0, 0, 0},
      "",
      wm::Task::Id{0},
      wm::Stack::Id::Default,
  };

  while (index < toast_windows_.size()) {
    auto tw = toast_windows_[index];
    if (tw != nullptr) {
      tw->update_state({update_window});
    }
    ++index;
  }
}
}  // namespace wm
}  // namespace anbox
