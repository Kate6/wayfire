#include <wayfire/plugin.hpp>
#include <wayfire/core.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/view.hpp>
#include <wayfire/window-manager.hpp>
#include <wayfire/output.hpp>

#include <wayfire/util/log.hpp>

namespace wf
{
/**
 * Focus follows mouse.
 *
 * Moves keyboard focus to whichever view the pointer enters. Wayfire has no
 * built-in option for this - the only focus-related keys in the metadata are
 * core/focus_buttons, core/focus_button_with_modifiers and simple-tile's
 * key_focus_*, none of which track the pointer - so this is a plugin.
 *
 * Focus is requested through window_manager_t::focus_request() rather than
 * seat->focus_view() so other plugins keep their veto and the usual focus
 * machinery (raise, output switching) runs unchanged.
 *
 * While any mouse button is held, focus is frozen. This is the important part:
 * every pointer-initiated grab (window move, resize, drag-and-drop) has a
 * button held for its duration, so this single flag suppresses the failure mode
 * where dragging a window across the screen keeps re-focusing everything it
 * passes over and the drag fights the focus change.
 */
class focus_follows_mouse_plugin_t : public plugin_interface_t
{
    // Milliseconds the pointer must rest on a view before focus moves.
    // 0 focuses immediately.
    option_wrapper_t<int> delay{"focus-follows-mouse/delay"};

    // Whether focus may cross to a view on a different output.
    option_wrapper_t<bool> cross_output{"focus-follows-mouse/cross_output"};

    /** True while any pointer button is held. Suppresses focus during drags. */
    bool button_held = false;

    /**
     * The view we most recently asked to focus. If focus did not land on it
     * (plugin veto, view closed, focus stolen) we do not ask again every frame.
     */
    wayfire_view last_requested = nullptr;

    /** Timestamp of the last motion event, for the delay. */
    int64_t motion_time = 0;

    /** True once any motion has been seen, so the first event starts a timer. */
    bool seen_motion = false;

    void handle_motion(wf::input_event_signal<wlr_pointer_motion_event> *ev)
    {
        // A button is down: this is a drag, a resize, or a dnd. Never move
        // focus here - see the class comment for why.
        if (this->button_held)
        {
            return;
        }

        auto view = wf::get_core().get_cursor_focus_view();
        if (!view)
        {
            // Pointer is over the desktop background. Leave focus where it is;
            // unfocusing is a separate policy and would be surprising here.
            return;
        }

        // Only genuine toplevels. Panel, docks and popups may be focusable, and
        // focusing one would pull the keyboard away from the window under it -
        // which is precisely the bug that makes the LXQt panel menu untypeable.
        if (!view->is_focusable() || !view->get_keyboard_focus_surface())
        {
            return;
        }

        auto seat = wf::get_core().seat.get();
        if (!seat)
        {
            return;
        }

        if (view == seat->get_active_view())
        {
            return;
        }

        if (view == this->last_requested)
        {
            return;
        }

        // Respect the configured delay, so that sweeping the pointer across a
        // row of windows does not focus all of them in turn.
        auto now = ev->event->time_msec;
        if (this->delay.value() > 0)
        {
            if (!this->seen_motion)
            {
                this->seen_motion = true;
                this->motion_time = now;
                return;
            }

            if ((now - this->motion_time) < this->delay.value())
            {
                return;
            }
        }

        this->motion_time = now;
        this->last_requested = view;

        if (!this->cross_output.value())
        {
            auto active_out = seat->get_active_output();
            auto view_out = view->get_output();
            if (active_out && view_out && (active_out != view_out))
            {
                return;
            }
        }

        auto wm = wf::get_core().default_wm.get();
        if (wm)
        {
            wm->focus_request(view);
        }
    }

    wf::signal::connection_t<wf::input_event_signal<wlr_pointer_motion_event>>
    on_motion = [=] (wf::input_event_signal<wlr_pointer_motion_event> *ev)
    {
        this->handle_motion(ev);
    };

    wf::signal::connection_t<wf::input_event_signal<wlr_pointer_button_event>>
    on_button = [=] (wf::input_event_signal<wlr_pointer_button_event> *ev)
    {
        this->button_held =
            (ev->event->state == WL_POINTER_BUTTON_STATE_PRESSED);
        this->last_requested = nullptr;
    };

  public:
    void init() override
    {
        wf::get_core().connect(&this->on_motion);
        wf::get_core().connect(&this->on_button);
    }

    void fini() override
    {
        wf::get_core().disconnect(&this->on_motion);
        wf::get_core().disconnect(&this->on_button);
    }
};
} // namespace wf

DECLARE_WAYFIRE_PLUGIN(wf::focus_follows_mouse_plugin_t);