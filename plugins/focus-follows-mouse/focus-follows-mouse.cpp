#include <wayfire/plugin.hpp>
#include <wayfire/core.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/view.hpp>
#include <wayfire/window-manager.hpp>
#include <wayfire/output.hpp>

#include <wayfire/util/log.hpp>

#include <cmath>

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

    // Raise the newly focused view above the others. Off by default: raising
    // every window the pointer merely passes over is the classic way
    // focus-follows-mouse becomes infuriating, since the window you glanced at
    // ends up covering the one you were working in.
    option_wrapper_t<bool> raise{"focus-follows-mouse/raise"};

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

        // Hit-test the scene directly rather than using
        // get_cursor_focus_view(). This handler runs on the *pre* input signal,
        // which Wayfire emits before update_cursor_focus() has been called for
        // this event (see pointer.cpp: emit_device_event_signal() precedes
        // update_cursor_focus()). So the cursor focus still names the view
        // under the pointer's *previous* position, and focus would trail the
        // pointer. get_view_at() hit-tests the scene now.
        auto pos = wf::get_core().get_cursor_position();
        if (std::isnan(pos.x) || std::isnan(pos.y))
        {
            return;
        }

        auto view = wf::get_core().get_view_at(pos);
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
        auto seat_ptr = wf::get_core().seat.get();
        if (!wm || !seat_ptr)
        {
            return;
        }

        if (this->raise.value())
        {
            // Goes through the signal, so other plugins can veto, and the
            // window is brought to the front.
            wm->focus_request(view);
            return;
        }

        // Default path: focus without raising.
        //
        // focus_request() always calls view_bring_to_front(), which is not
        // exposed in the public window-manager header, so there is no way to
        // undo it afterwards. seat->focus_view() skips the raise entirely.
        //
        // The trade-off is that focus_view() does not emit
        // view_focus_request_signal, so a plugin that wants to veto focus
        // changes (a kiosk plugin, a modal-dialog handler) will not see ours.
        // Focus still switches outputs first, matching what focus_raise_view()
        // does, so per-output plugin state stays correct.
        if (auto view_out = view->get_output())
        {
            seat_ptr->focus_output(view_out);
        }

        seat_ptr->focus_view(view);
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