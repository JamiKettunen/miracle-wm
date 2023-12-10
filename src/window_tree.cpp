/*
 * Copyright © Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2 or 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "window_tree.h"
#include <memory>
#include <mir/log.h>
#include <iostream>
#include <cmath>

using namespace miracle;

WindowTree::WindowTree(geom::Size default_size)
    : root_lane{std::make_shared<Node>(geom::Rectangle{geom::Point{}, default_size})},
      active_lane{root_lane},
      size{default_size}
{
}

miral::WindowSpecification WindowTree::allocate_position(const miral::WindowSpecification &requested_specification)
{
    miral::WindowSpecification new_spec = requested_specification;

    // Everyone get out the damn way. Slide the other items to the left for now.
    // TODO: Handle inserting in the middle of the group
    // TODO: Handle non-equal sizing
    auto rectangle = active_lane->get_rectangle();
    if (active_lane->get_direction() == NodeDirection::horizontal)
    {
        auto width_per_item = rectangle.size.width.as_int() / static_cast<float>(active_lane->get_sub_nodes().size() + 1);
        auto new_size = geom::Size{geom::Width{width_per_item}, rectangle.size.height};
        auto new_position = geom::Point{
            rectangle.top_left.x.as_int() + rectangle.size.width.as_int() - width_per_item,
            rectangle.top_left.y
        };
        new_spec.top_left() = new_position;
        new_spec.size() = new_size;
    }
    else if (active_lane->get_direction() == NodeDirection::vertical)
    {
        auto height_per_item = rectangle.size.height.as_int() / static_cast<float>(active_lane->get_sub_nodes().size() + 1);
        auto new_size = geom::Size{rectangle.size.width, height_per_item};
        auto new_position = geom::Point{
            rectangle.top_left.x,
            rectangle.top_left.y.as_int() + rectangle.size.height.as_int() - height_per_item
        };
        new_spec.top_left() = new_position;
        new_spec.size() = new_size;
    }

    return new_spec;
}

void WindowTree::confirm(miral::Window &window)
{
    geom::Rectangle rectangle = active_lane->get_rectangle();
    auto nodes = active_lane->get_sub_nodes();
    if (active_lane->get_direction() == NodeDirection::horizontal)
    {
        float divvied = window.size().width.as_int() / static_cast<float>(nodes.size());
        std::shared_ptr<Node> prev_node;
        for (auto node : nodes)
        {
            auto node_rect = node->get_rectangle();
            node_rect.size.width = geom::Width{node_rect.size.width.as_int() - divvied};

            if (prev_node)
            {
                node_rect.top_left.x = geom::X{
                    prev_node->get_rectangle().top_left.x.as_int() + prev_node->get_rectangle().size.width.as_int()};
            }

            node->set_rectangle(node_rect);
            prev_node = node;
        }
    }
    else
    {
        float divvied = window.size().height.as_int() / static_cast<float>(nodes.size());
        std::shared_ptr<Node> prev_node;
        for (auto node : nodes)
        {
            auto node_rect = node->get_rectangle();
            node_rect.size.height = geom::Height {node_rect.size.height.as_int() - divvied};

            if (prev_node)
            {
                node_rect.top_left.y = geom::Y{
                    prev_node->get_rectangle().top_left.y.as_int() + prev_node->get_rectangle().size.height.as_int()};
            }

            node->set_rectangle(node_rect);
            prev_node = node;
        }
    }

    active_lane->get_sub_nodes().push_back(std::make_shared<Node>(
        geom::Rectangle{window.top_left(), window.size()},
        active_lane,
        window));

    active_lane->set_rectangle(rectangle);
    advise_focus_gained(window);
}

void WindowTree::toggle_resize_mode()
{
    if (is_resizing)
    {
        is_resizing = false;
        return;
    }

    auto window_lane = root_lane->find_node_for_window(active_window);
    if (!window_lane)
        return;

    is_resizing = true;
}

bool WindowTree::try_resize_active_window(miracle::WindowResizeDirection direction)
{
    if (!is_resizing)
        return false;

    auto window_lane = root_lane->find_node_for_window(active_window);
    if (!window_lane->is_window())
    {
        std::cerr << "WindowTree::try_resize_active_window: unable to resize a non-window" << std::endl;
        return false;
    }

    // TODO: We have a hardcoded resize amount
    resize_node_internal(window_lane, direction, 50);
    return true;
}

void WindowTree::resize_node_internal(
    std::shared_ptr<Node> node,
    WindowResizeDirection direction,
    int amount)
{
    // Behavior:
    // Case #1: When resizing in a direction, we expand the node in that direction,
    // thus shrinking every other node in the list to accommodate the new
    // size of our node. The offset size is spread equally throughout the
    // other nodes, such that they all lose size the same.
    // Case #2: when a node is the first or last in the list, and we resize
    // in a direction that would make it hit the wall, then we decrease the size
    // of the node, and add size to the other nodes.
    // Case #3: If we resize in the opposite direction of one we were resizing in
    // then, we shrink the node.

    // Algorithm: If we're resizing along the main axis of the window,
    // (e.g. we are going up/down and the direction of the axis is
    // vertical), then we will not resize the parent node itself. Rather,
    // we will resize the other nodes in the lane in relation to this
    // resized window.
    // However, if we're resizing along the cross axis of the window
    // (e.g. the horizontal axis in the previous example), then we
    // resize the parent horizontally, and resize the other nodes around it.
    // This is obviously recursive.
    auto parent = node->parent;
    if (parent == nullptr)
    {
        // Can't resize, most likely the root
        return;
    }

    // Discover the node's position
    bool is_vertical = direction == WindowResizeDirection::up || direction == WindowResizeDirection::down;
    bool is_main_axis_movement = (is_vertical  && parent->get_direction() == NodeDirection::vertical)
        || (!is_vertical && parent->get_direction() == NodeDirection::horizontal);
    if (!is_main_axis_movement)
    {
        resize_node_internal(parent, direction, amount);
        return;
    }

    bool is_negative = direction == WindowResizeDirection::left || direction == WindowResizeDirection::up;
    auto resize_amount = is_negative ? -amount : amount;
    auto nodes = parent->get_sub_nodes();
    if (is_vertical)
    {
        int height_for_others = floor(-resize_amount / static_cast<float>(nodes.size() - 1));
        for (size_t i = 0; i < nodes.size(); i++)
        {
            auto other_node = nodes[i];
            auto other_rect = other_node->get_rectangle();
            if (node == other_node)
                other_rect.size.height = geom::Height{other_rect.size.height.as_int() + resize_amount};
            else
                other_rect.size.height = geom::Height{other_rect.size.height.as_int() + height_for_others};

            if (i != 0)
            {
                auto prev_rect = nodes[i - 1]->get_rectangle();
                other_rect.top_left.y = geom::Y{prev_rect.top_left.y.as_int() + prev_rect.size.height.as_int()};
            }
            other_node->set_rectangle(other_rect);
        }
    }
    else
    {
        int width_for_others = floor(-resize_amount / static_cast<float>(nodes.size() - 1));
        for (size_t i = 0; i < nodes.size(); i++)
        {
            auto other_node = nodes[i];
            auto other_rect = other_node->get_rectangle();
            if (node == other_node)
                other_rect.size.width = geom::Width {other_rect.size.width.as_int() + resize_amount};
            else
                other_rect.size.width = geom::Width {other_rect.size.width.as_int() + width_for_others};

            if (i != 0)
            {
                auto prev_rect = nodes[i - 1]->get_rectangle();
                other_rect.top_left.x = geom::X{prev_rect.top_left.x.as_int() + prev_rect.size.width.as_int()};
            }
            other_node->set_rectangle(other_rect);
        }
    }
}

void WindowTree::resize(geom::Size new_size)
{
    size = new_size;
    // TODO: Resize all windows
}

void WindowTree::request_vertical()
{
    handle_direction_request(NodeDirection::vertical);
}

void WindowTree::request_horizontal()
{
    handle_direction_request(NodeDirection::horizontal);
}

void WindowTree::handle_direction_request(NodeDirection direction)
{
    if (is_resizing)
        return;

    auto window_lane = active_lane->find_node_for_window(active_window);
    if (!window_lane->is_window())
    {
        std::cerr << "WindowTree::handle_direction_request: must change direction of a window" << std::endl;
        return;
    }

    // Edge case: if we're an only child, we can just change the direction of the parent to achieve the same effect
    if (window_lane->parent->get_sub_nodes().size() == 1)
    {
        active_lane = window_lane->parent;
    }
    else
    {
        active_lane = window_lane;
        active_lane->to_lane();
    }

    active_lane->set_direction(direction);
}

void WindowTree::advise_focus_gained(miral::Window& window)
{
    // The node that we find will be the window, so its parent must be the lane
    auto found_node = root_lane->find_node_for_window(window);
    active_lane = found_node->parent;
    if (!active_lane->is_lane())
    {
        std::cerr << "Active lane is NOT a lane" << std::endl;
    }
    active_window = window;
}

void WindowTree::advise_focus_lost(miral::Window& window)
{
}

void WindowTree::advise_delete_window(miral::Window& window)
{
    // Capture the previous size before anything starts
    auto rectangle = active_lane->get_rectangle();

    // Resize the other nodes in the lane accordingly
    auto lane = root_lane->find_node_for_window(window);
    if (!lane)
    {
        std::cerr << "Unable to find lane for window" << std::endl;
        return;
    }

    if (!lane->is_window())
    {
        std::cerr << "Lane should have been a window" << std::endl;
        return;
    }

    active_lane = lane->parent;

    if (active_lane->get_sub_nodes().size() == 1)
    {
        // Remove the entire lane if this lane is now empty
        if (active_lane->parent)
        {
            auto prev_active = active_lane;
            active_lane = active_lane->parent;
            rectangle = active_lane->get_rectangle(); // Note: The rectangle needs to point to the new active to be correct.

            active_lane->get_sub_nodes().erase(
                std::remove_if(active_lane->get_sub_nodes().begin(), active_lane->get_sub_nodes().end(), [&](std::shared_ptr<Node> content) {
                    return content->is_lane() && content == prev_active;
                }),
                active_lane->get_sub_nodes().end()
            );
        }
    }
    else
    {
        // Remove the window from the active lane
        active_lane->get_sub_nodes().erase(
            std::remove_if(active_lane->get_sub_nodes().begin(), active_lane->get_sub_nodes().end(), [&](std::shared_ptr<Node> content) {
                return content->is_window() && content->get_window() == window;
            }),
            active_lane->get_sub_nodes().end()
        );
    }

    // Edge case: If the newly active node only owns one other lane, it can absorb the node
    if (active_lane->get_sub_nodes().size() == 1 && active_lane->get_sub_nodes()[0]->is_lane())
    {
        auto dying_lane = active_lane->get_sub_nodes()[0];
        active_lane->get_sub_nodes().clear();
        for (auto sub_node : dying_lane->get_sub_nodes())
        {
            active_lane->get_sub_nodes().push_back(sub_node);
        }
        active_lane->set_direction(dying_lane->get_direction());
    }

    active_lane->set_rectangle(rectangle);
}