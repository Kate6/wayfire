#include "wayfire/signal-provider.hpp"
#include "wayfire/toplevel-view.hpp"
#include <wayfire/debug.hpp>
#include "wayfire/txn/transaction-manager.hpp"
#include "wayfire/plugin.hpp"
#include <wayfire/view.hpp>
#include <wayfire/view-helpers.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/core.hpp>
#include <wayfire/workarea.hpp>
#include <wayfire/window-manager.hpp>
#include <wayfire/signal-definitions.hpp>

class wayfire_place_cascade_data : public wf::custom_data_t
{
  public:
    int x = 0;
    int y = 0;
};

class wayfire_place_window : public wf::plugin_interface_t
{
    wf::signal::connection_t<wf::txn::new_transaction_signal> on_new_tx =
        [=] (wf::txn::new_transaction_signal *ev)
    {
        // For each transaction, we need to consider what happens with participating views
        for (const auto& obj : ev->tx->get_objects())
        {
            auto toplevel = std::dynamic_pointer_cast<wf::toplevel_t>(obj);
            if (!toplevel || !map_pending(toplevel))
            {
                continue;
            }

            auto view = wf::find_view_for_toplevel(toplevel);
            if (view && should_place(view))
            {
                do_place(view);
            }
        }
    };

    bool map_pending(std::shared_ptr<wf::toplevel_t> toplevel)
    {
        return !toplevel->current().mapped && toplevel->pending().mapped;
    }

    bool should_place(wayfire_toplevel_view toplevel)
    {
        if (toplevel->parent)
        {
            return false;
        }

        if (toplevel->pending_fullscreen() || toplevel->pending_tiled_edges())
        {
            return false;
        }

        if (toplevel->has_property("startup-x") || toplevel->has_property("startup-y"))
        {
            return false;
        }

        if (!toplevel->get_output())
        {
            return false;
        }

        return true;
    }

    void do_place(wayfire_toplevel_view view)
    {
        auto output   = view->get_output();
        auto workarea = output->workarea->get_workarea();

        std::string mode = placement_mode;
        if (mode == "cascade")
        {
            cascade(view, workarea);
        } else if (mode == "smart")
        {
            smart(view, workarea);
        } else if (mode == "maximize")
        {
            maximize(view, workarea);
        } else if (mode == "random")
        {
            random(view, workarea);
        } else if (mode == "pointer")
        {
            pointer(view, workarea);
        } else
        {
            center(view, workarea);
        }
    }

    void adjust_cascade_for_workarea(nonstd::observer_ptr<wayfire_place_cascade_data> cascade,
        wf::geometry_t workarea)
    {
        if ((cascade->x < workarea.x) || (cascade->x > workarea.x + workarea.width))
        {
            cascade->x = workarea.x;
        }

        if ((cascade->y < workarea.y) || (cascade->y > workarea.y + workarea.height))
        {
            cascade->y = workarea.y;
        }
    }

    wf::option_wrapper_t<std::string> placement_mode{"place/mode"};

  public:
    void init() override
    {
        wf::get_core().tx_manager->connect(&on_new_tx);
    }

    void cascade(wayfire_toplevel_view view, wf::geometry_t workarea)
    {
        wf::geometry_t window = view->get_pending_geometry();
        auto cascade = view->get_output()->get_data_safe<wayfire_place_cascade_data>();
        adjust_cascade_for_workarea(cascade, workarea);

        if ((cascade->x + window.width > workarea.x + workarea.width) ||
            (cascade->y + window.height > workarea.y + workarea.height))
        {
            cascade->x = workarea.x;
            cascade->y = workarea.y;
        }

        view->toplevel()->pending().geometry.x = cascade->x;
        view->toplevel()->pending().geometry.y = cascade->y;

        cascade->x += workarea.width * .03;
        cascade->y += workarea.height * .03;
    }

    void random(wayfire_toplevel_view & view, wf::geometry_t workarea)
    {
        wf::geometry_t window = view->get_pending_geometry();
        wf::geometry_t area;

        area.x     = workarea.x;
        area.y     = workarea.y;
        area.width = workarea.width - window.width;
        area.height = workarea.height - window.height;

        if ((area.width <= 0) || (area.height <= 0))
        {
            center(view, workarea);

            return;
        }

        view->toplevel()->pending().geometry.x = area.x +
            (rand() % std::max(1, (int)std::ceil(area.width)));
        view->toplevel()->pending().geometry.y = area.y +
            (rand() % std::max(1, (int)std::ceil(area.height)));
    }

    void center(wayfire_toplevel_view & view, wf::geometry_t workarea)
    {
        wf::geometry_t window = view->get_pending_geometry();
        view->toplevel()->pending().geometry.x = workarea.x + (workarea.width / 2) - (window.width / 2);
        view->toplevel()->pending().geometry.y = workarea.y + (workarea.height / 2) - (window.height / 2);
    }

    void pointer(wayfire_toplevel_view & view, wf::geometry_t workarea)
    {
        wf::output_t *output = view->get_output();
        if (!output)
        {
            return;
        }

        wf::pointf_t pos = output->get_cursor_position();
        wf::geometry_t window = view->get_pending_geometry();
        window.x = workarea.x + std::clamp(pos.x - window.width / 2, 0.0, workarea.width - window.width);
        window.y = workarea.y + std::clamp(pos.y - window.height / 2, 0.0, workarea.height - window.height);
        view->toplevel()->pending().geometry.x = window.x;
        view->toplevel()->pending().geometry.y = window.y;
    }

    void maximize(wayfire_toplevel_view & view, wf::geometry_t workarea)
    {
        wf::get_core().default_wm->tile_request(view, wf::TILED_EDGES_ALL);
    }

    /**
     * "Smart" placement, after Compiz's placeSmart() (plugins/place.c).
     *
     * Rather than scanning for a free slot, this scores candidate positions by
     * how much they would overlap existing windows and picks the least-bad one,
     * stepping past each obstacle in turn. Overlap with a window that is above
     * the new one counts sixteen times as much, and a window below counts
     * nothing - so the result slides under things rather than over them.
     *
     * The algorithm is credited in the original to SmartPlacement by Cristian
     * Tibirna, adapted through kwm, kwin, fvwm and xfce before reaching Compiz.
     */
    void smart(wayfire_toplevel_view view, wf::geometry_t workarea)
    {
        // Compiz's sentinels: NONE means "no overlap", the negatives mean the
        // candidate does not fit in that axis at all.
        const int NONE = 0;
        const int H_WRONG = -1;
        const int W_WRONG = -2;

        struct obstacle
        {
            wf::geometry_t g;
            int weight;
        };

        auto output = view->get_output();
        if (!output)
        {
            return;
        }

        // Gather the windows we should avoid. The layer a view sits in is the
        // closest equivalent of Compiz's above/below window state, which
        // Wayfire's public API does not otherwise expose.
        std::vector<obstacle> obstacles;
        auto collect = [&] (std::initializer_list<wf::scene::layer> layers, int weight)
        {
            for (auto candidate : wf::collect_views_from_output(output, layers))
            {
                auto toplevel = wf::toplevel_cast(candidate);
                if (!toplevel || toplevel == view)
                {
                    continue;
                }

                obstacles.push_back({toplevel->get_pending_geometry(), weight});
            }
        };

        // Always-on-top windows count heavily, always-below ones not at all.
        collect({wf::scene::layer::TOP}, 16);
        collect({wf::scene::layer::WORKSPACE}, 1);
        collect({wf::scene::layer::BOTTOM}, 0);

        wf::geometry_t window = view->get_pending_geometry();
        int cw = window.width;
        int ch = window.height;

        if ((cw <= 0) || (ch <= 0) || (cw >= workarea.width) || (ch >= workarea.height))
        {
            // Too big to place meaningfully; cascade or centre instead.
            center(view, workarea);

            return;
        }

        int x_tmp  = workarea.x;
        int y_tmp  = workarea.y;
        int x_optimal = x_tmp;
        int y_optimal = y_tmp;

        int overlap = NONE;
        int min_overlap = 0;
        bool first_pass = true;

        do
        {
            if (y_tmp + ch > workarea.y + workarea.height)
            {
                // No vertical room left: bail out, keeping the best found so far.
                overlap = H_WRONG;
            } else if (x_tmp + cw > workarea.x + workarea.width)
            {
                overlap = W_WRONG;
            } else
            {
                overlap = NONE;
                int cxl = x_tmp;
                int cxr = x_tmp + cw;
                int cyt = y_tmp;
                int cyb = y_tmp + ch;

                for (const auto& o : obstacles)
                {
                    int xl = o.g.x;
                    int yt = o.g.y;
                    int xr = o.g.x + o.g.width;
                    int yb = o.g.y + o.g.height;

                    if ((cxl < xr) && (cxr > xl) && (cyt < yb) && (cyb > yt))
                    {
                        int ixl = std::max(cxl, xl);
                        int ixr = std::min(cxr, xr);
                        int iyt = std::max(cyt, yt);
                        int iyb = std::min(cyb, yb);
                        overlap += o.weight * (ixr - ixl) * (iyb - iyt);
                    }
                }
            }

            if (overlap == NONE)
            {
                // No overlap at all - take it immediately.
                x_optimal = x_tmp;
                y_optimal = y_tmp;

                break;
            }

            if (first_pass)
            {
                first_pass  = false;
                min_overlap = overlap;
            } else if ((overlap > NONE) && (overlap < min_overlap))
            {
                min_overlap = overlap;
                x_optimal = x_tmp;
                y_optimal = y_tmp;
            }

            if (overlap > NONE)
            {
                // Step right to the nearest edge that clears everything we
                // currently overlap.
                int possible = workarea.x + workarea.width;
                if (possible - cw > x_tmp)
                {
                    possible -= cw;
                }

                for (const auto& o : obstacles)
                {
                    int xl = o.g.x;
                    int yt = o.g.y;
                    int xr = o.g.x + o.g.width;
                    int yb = o.g.y + o.g.height;

                    if ((y_tmp < yb) && (yt < ch + y_tmp))
                    {
                        if ((xr > x_tmp) && (possible > xr))
                        {
                            possible = xr;
                        }

                        int basket = xl - cw;
                        if ((basket > x_tmp) && (possible > basket))
                        {
                            possible = basket;
                        }
                    }
                }

                x_tmp = possible;
            } else if (overlap == W_WRONG)
            {
                // Out of horizontal room: restart from the left and step down.
                x_tmp     = workarea.x;
                int possible = workarea.y + workarea.height;
                if (possible - ch > y_tmp)
                {
                    possible -= ch;
                }

                for (const auto& o : obstacles)
                {
                    int yt = o.g.y;
                    int yb = o.g.y + o.g.height;

                    if ((yb > y_tmp) && (possible > yb))
                    {
                        possible = yb;
                    }

                    int basket = yt - ch;
                    if ((basket > y_tmp) && (possible > basket))
                    {
                        possible = basket;
                    }
                }

                y_tmp = possible;
            }
        }
        while ((overlap != NONE) && (overlap != H_WRONG) &&
            (y_tmp < workarea.y + workarea.height));

        view->toplevel()->pending().geometry.x = x_optimal;
        view->toplevel()->pending().geometry.y = y_optimal;
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_place_window);
