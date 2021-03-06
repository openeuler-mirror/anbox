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

#ifndef ANBOX_WM_MANAGER_H_
#define ANBOX_WM_MANAGER_H_

#include "anbox/wm/window.h"
#include "anbox/wm/window_state.h"

#include <map>
#include <memory>
#include <mutex>

namespace anbox {
namespace wm {
class Manager {
 public:
  virtual ~Manager();

  virtual void setup() {}

  virtual void apply_window_state_update(const WindowState::List &updated, const WindowState::List &removed) = 0;

  virtual void resize_task(const Task::Id &task, const anbox::graphics::Rect &rect,
                           const std::int32_t &resize_mode) = 0;
  virtual void set_focused_task(const Task::Id &task) = 0;
  virtual void remove_task(const Task::Id &task) = 0;
  virtual void insert_task(const Task::Id &task, std::shared_ptr<wm::Window> pt) = 0;
  virtual void erase_task(const Task::Id &task) = 0;
  virtual std::shared_ptr<Window> get_toast_window(const anbox::graphics::Rect &rect, int index) = 0;
  virtual void hide_rest_toast_window(int index) = 0;

  virtual std::string get_title(const std::string &package_name) = 0;
  // FIXME only applies for the multi-window case
  virtual std::shared_ptr<Window> find_window_for_task(const Task::Id &task) = 0;
  virtual void add_toast_window(std::shared_ptr<wm::Window> tw) = 0;
};
}  // namespace wm
}  // namespace anbox

#endif
