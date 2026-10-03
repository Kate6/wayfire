/*
 * Compositor-side EIS (emulated input server) for Wayfire.
 *
 * This plugin owns the only thing that can satisfy the InputCapture portal's
 * ConnectToEIS() requirement: eis_backend_fd_add_client() is a libei*s* (server
 * side) API, so the fd handed to the application has to be minted here and passed
 * to the portal over a private D-Bus interface.
 *
 * The private interface mirrors mutter's org.gnome.Mutter.InputCapture: the portal
 * (xdg-desktop-portal-wlr) is a translation shim on top of it.
 *
 * Each session owns its own libei *server* context. That is what lets us attribute
 * an incoming EIS client to a session: eis_backend_fd_add_client() returns only an
 * fd, not the eis_client *, so a shared context would leave us unable to tell
 * which session a client belongs to. mutter takes the same approach.
 */

#include "eis.hpp"

// System headers that must not see Wayfire's macro shims come first.
#include <systemd/sd-bus.h>
#include <libeis.h>

#include <wayfire/core.hpp>
#include <wayfire/input-device.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/util.hpp>

#include <wayfire/nonstd/wlroots-full.hpp>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unistd.h>

namespace
{
constexpr const char *DBUS_SERVICE = "org.wayfire.Eis";
constexpr const char *DBUS_MANAGER_IFACE = "org.wayfire.Eis";
constexpr const char *DBUS_MANAGER_PATH = "/org/wayfire/Eis";
constexpr const char *DBUS_SESSION_IFACE = "org.wayfire.Eis.Session";

/** strerror() as a std::string, for Wayfire's concatenating LOG* macros. */
std::string errstr(int err)
{
    return std::string(strerror(err));
}

/** Small helper so LOG* calls can format integers without std::hex games. */
std::string hex(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", v);
    return std::string(buf);
}

/**
 * Route libei log messages into Wayfire's logger.
 *
 * libei's own default handler writes to stderr, which in a compositor is easy to
 * lose.
 */
void eis_log_trampoline(struct eis *eis, enum eis_log_priority priority,
    const char *message, struct eis_log_context *ctx)
{
    // Wayfire's LOG* macros concatenate their arguments; they are not
    // printf-style, and they ignore stream manipulators such as std::hex.
    switch (priority)
    {
      case EIS_LOG_PRIORITY_DEBUG:
        LOGD("eis: ", message);
        break;
      case EIS_LOG_PRIORITY_INFO:
        LOGI("eis: ", message);
        break;
      case EIS_LOG_PRIORITY_WARNING:
        LOGW("eis: ", message);
        break;
      case EIS_LOG_PRIORITY_ERROR:
        LOGE("eis: ", message);
        break;
    }
}

/**
 * How a barrier line relates to one zone. Ported from mutter's
 * get_barrier_adjacency(), which is the reference for the spec's rules that a
 * barrier must lie on the outside boundary of the union of zones and must be
 * fully contained within exactly one zone.
 */
enum class adjacency_t
{
    NONE,       ///< no relation
    OVERLAP,    ///< the line runs through the interior of the zone
    CONTAINED,  ///< the line lies exactly on one edge of this zone
    PARTIAL,    ///< the line is on an edge but runs past its end
};

adjacency_t barrier_adjacency(const zone_t &z, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2)
{
    const int32_t x_min = std::min(x1, x2);
    const int32_t x_max = std::max(x1, x2);
    const int32_t y_min = std::min(y1, y2);
    const int32_t y_max = std::max(y1, y2);

    if (x1 == x2)
    {
        // Vertical barrier.
        const int32_t x = x1;
        if (x < z.x || x > z.x + z.width)
        {
            return adjacency_t::NONE;
        }

        if (y_max < z.y || y_min >= z.y + z.height)
        {
            return adjacency_t::NONE;
        }

        if (z.x + z.width == x || z.x == x)
        {
            if (y_max > z.y + z.height || y_min < z.y)
            {
                return adjacency_t::PARTIAL;
            }

            return adjacency_t::CONTAINED;
        }

        return adjacency_t::OVERLAP;
    }

    if (y1 == y2)
    {
        // Horizontal barrier.
        const int32_t y = y1;
        if (y < z.y || y > z.y + z.height)
        {
            return adjacency_t::NONE;
        }

        if (x_max < z.x || x_min >= z.x + z.width)
        {
            return adjacency_t::NONE;
        }

        if (z.y + z.height == y || z.y == y)
        {
            if (x_max > z.x + z.width || x_min < z.x)
            {
                return adjacency_t::PARTIAL;
            }

            return adjacency_t::CONTAINED;
        }

        return adjacency_t::OVERLAP;
    }

    return adjacency_t::NONE;
}


// ---------------------------------------------------------------------------
// D-Bus vtable handlers
// ---------------------------------------------------------------------------

int handle_get_supported_capabilities(sd_bus *bus, const char *object,
    const char *iface, const char *property, sd_bus_message *msg, void *userdata,
    sd_bus_error *error);
int handle_create_session(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error);
int handle_get_zones(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error);
int handle_add_barrier(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error);
int handle_clear_barriers(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error);
int handle_enable(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error);
int handle_disable(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error);
int handle_release(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error);
int handle_connect_to_eis(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error);
int handle_session_close(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error);

const sd_bus_vtable manager_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("SupportedCapabilities", "u",
        handle_get_supported_capabilities, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("CreateSession", "u", "o", handle_create_session,
        SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

const sd_bus_vtable session_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("ConnectToEIS", "", "h", handle_connect_to_eis,
        SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetZones", "", "ua(uuii)", handle_get_zones,
        SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AddBarrier", "u(iiii)", "u", handle_add_barrier,
        SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ClearBarriers", "", "", handle_clear_barriers,
        SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Enable", "", "", handle_enable, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Disable", "", "", handle_disable, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release", "a{sv}", "", handle_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Close", "", "", handle_session_close, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Activated", "uudd", 0),
    SD_BUS_SIGNAL("Deactivated", "u", 0),
    SD_BUS_SIGNAL("ZonesChanged", "", 0),
    SD_BUS_SIGNAL("Disabled", "", 0),
    SD_BUS_SIGNAL("Closed", "", 0),
    SD_BUS_VTABLE_END
};

} // namespace

// ---------------------------------------------------------------------------
// eis_session_t
// ---------------------------------------------------------------------------

eis_session_t::eis_session_t(eis_server_t *server, std::string path,
    uint32_t capabilities, std::string peer, uint32_t session_id) :
    srv(server),
    object_path(std::move(path)),
    peer_name(std::move(peer)),
    caps(capabilities),
    id(session_id)
{
    this->refresh_zones();
}

eis_session_t::~eis_session_t()
{
    this->close();
}

bool eis_session_t::check_permission(sd_bus_message *msg)
{
    const char *sender = sd_bus_message_get_sender(msg);
    if (!sender)
    {
        return false;
    }

    // Modeled on mutter's check_permission(): only the unique bus name that
    // created the session may operate on it. Note that our caller is the portal,
    // not the application -- the portal performs the app-identity check.
    return this->peer_name == sender;
}

void eis_session_t::close()
{
    if (this->eis_source)
    {
        wl_event_source_remove(this->eis_source);
        this->eis_source = nullptr;
    }

    this->deactivate(false);

    if (this->seat)
    {
        eis_seat_remove(this->seat);
        eis_seat_unref(this->seat);
        this->seat = nullptr;
    }

    if (this->context)
    {
        eis_unref(this->context);
        this->context = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Zones and barriers
// ---------------------------------------------------------------------------

void eis_session_t::refresh_zones()
{
    std::vector<zone_t> fresh;

    for (auto *out : wf::get_core().output_layout->get_outputs())
    {
        auto g = out->get_layout_geometry();
        fresh.push_back(zone_t{g.x, g.y, g.width, g.height});
    }

    if (fresh == this->zone_list)
    {
        return;
    }

    this->zone_list = fresh;
    for (const auto &z : fresh)
    {
        LOGI("zone ", z.width, "x", z.height, " at ", z.x, ",", z.y);
    }

    // The spec requires the id to increase by a sensible amount so that clients
    // can detect wraparound.
    this->zone_serial += 3;

    // Any barrier that no longer lies on the zone boundary is meaningless, and
    // the spec requires the session to be suspended when zones change.
    if (!this->barriers.empty())
    {
        this->clear_barriers();
    }
    else
    {
        this->emit_zones_changed();
    }
}

uint32_t eis_session_t::add_barrier(uint32_t serial, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2)
{
    // Barriers may only be added while INIT; setting barriers implicitly
    // suspends an enabled session (mutter refuses for the same reason).
    if (this->session_state != session_state_t::INIT)
    {
        LOGW("eis: AddBarrier rejected, session is not INIT");
        return 0;
    }

    this->refresh_zones();

    if (serial != this->zone_serial)
    {
        LOGW("eis: AddBarrier rejected, zone set ", serial,
            " is stale (current ", this->zone_serial, ")");
        return 0;
    }

    if (x1 != x2 && y1 != y2)
    {
        LOGW("eis: AddBarrier rejected, not axis aligned");
        return 0;
    }

    if (x1 == x2 && y1 == y2)
    {
        LOGW("eis: AddBarrier rejected, zero length");
        return 0;
    }

    // A barrier must lie on the outside boundary of the union of zones and be
    // fully contained within exactly one zone. This is what makes an internal
    // edge between two adjacent monitors invalid -- it is contained in two.
    bool contained_once = false;
    int ox = 0;
    int oy = 0;
    for (const auto &z : this->zone_list)
    {
        switch (barrier_adjacency(z, x1, y1, x2, y2))
        {
          case adjacency_t::CONTAINED:
            if (contained_once)
            {
                LOGW("eis: AddBarrier rejected, adjacent to multiple monitor edges");
                return 0;
            }

            contained_once = true;

            // Remember which way is "outside", so crossing detection knows
            // which side the desktop is on.
            if (x1 == x2)
            {
                ox = (z.x == x1) ? -1 : 1;
            }
            else
            {
                oy = (z.y == y1) ? -1 : 1;
            }

            break;

          case adjacency_t::OVERLAP:
            LOGW("eis: AddBarrier rejected, line overlaps a zone");
            return 0;

          case adjacency_t::PARTIAL:
            LOGW("eis: AddBarrier rejected, line partially covers a zone");
            return 0;

          case adjacency_t::NONE:
            break;
        }
    }

    if (!contained_once)
    {
        LOGW("eis: AddBarrier rejected, not adjacent to any zone");
        return 0;
    }

    // A horizontal barrier must not extend into the non-existent space to the
    // right of the rightmost zone. This is mutter's "extends into nonexisting
    // monitor region" rule, expressed over our zone list.
    if (y1 == y2 && !this->zone_list.empty())
    {
        int32_t right = 0;
        bool first = true;
        for (const auto &z : this->zone_list)
        {
            if (first || z.x + z.width > right)
            {
                right = z.x + z.width;
                first = false;
            }
        }

        zone_t phantom{right, y1 - 1, 1, 1};
        if (barrier_adjacency(phantom, x1, y1, x2, y2) != adjacency_t::NONE)
        {
            LOGW("eis: AddBarrier rejected, extends into a non-existent zone");
            return 0;
        }
    }

    barrier_t b;
    b.id = this->next_barrier_id++;
    b.x1 = x1;
    b.y1 = y1;
    b.x2 = x2;
    b.y2 = y2;
    b.ox = ox;
    b.oy = oy;
    this->barriers[b.id] = b;

    LOGI("eis: added barrier ", b.id, " (", x1, ",", y1, ")-(", x2, ",", y2, ")");
    return b.id;
}

void eis_session_t::clear_barriers()
{
    if (this->barriers.empty() && this->session_state == session_state_t::INIT)
    {
        return;
    }

    this->barriers.clear();

    // Setting barriers immediately suspends the session.
    if (this->session_state != session_state_t::INIT)
    {
        const bool was_active = (this->session_state == session_state_t::ACTIVATED);
        this->deactivate(was_active);
    }

    this->emit_zones_changed();
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void eis_session_t::enable()
{
    if (this->session_state != session_state_t::INIT)
    {
        return;
    }

    if (this->barriers.empty())
    {
        LOGW("eis: Enable with no barriers; capture can never activate");
    }

    this->session_state = session_state_t::ENABLED;
    LOGI("eis: session enabled, awaiting a barrier crossing");
}

void eis_session_t::disable()
{
    if (this->session_state == session_state_t::INIT)
    {
        return;
    }

    this->deactivate(true);
    this->session_state = session_state_t::INIT;
}

void eis_session_t::release(uint32_t requested_activation)
{
    if (this->session_state != session_state_t::ACTIVATED)
    {
        return;
    }

    // A stale Release for an already-finished capture must be ignored, so that a
    // deactivation racing a new activation is not torn down by the old client.
    if (requested_activation != 0 && requested_activation != this->activation_id)
    {
        LOGI("eis: ignoring stale Release for activation ",
            requested_activation, " (current ", this->activation_id, ")");
        return;
    }

    this->deactivate(true);
    this->session_state = session_state_t::ENABLED;
}

void eis_session_t::activate(uint32_t barrier_id, double cx, double cy)
{
    // Start emulation on every bound device *before* signalling Activated, so a
    // client that sees Activated is guaranteed the EI stream is live. The spec
    // requires activation_id and the EI start_emulating sequence to match.
    this->activation_id = ++this->sequence;

    for (auto *dev : {this->pointer, this->absolute, this->keyboard})
    {
        if (dev)
        {
            eis_device_start_emulating(dev, this->activation_id);
        }
    }

    this->session_state = session_state_t::ACTIVATED;

    /*
     * While the capture is active the pointer is driving the *remote* machine, so
     * the local cursor must not be drawn: leaving it visible leaves a stray cursor
     * pinned against the zone boundary where the pointer was last seen. Pair with
     * deactivate() below; hide_cursor() is refcounted, so this must be balanced.
     */
    if (!this->cursor_hidden)
    {
        wf::get_core().hide_cursor();
        this->cursor_hidden = true;
    }

    LOGI("eis: activated by barrier ", barrier_id, " activation_id=",
        this->activation_id, " cursor=", cx, ",", cy);
    this->emit_activated(barrier_id, cx, cy);
}

void eis_session_t::deactivate(bool emit_signal)
{
    if (this->session_state != session_state_t::ACTIVATED)
    {
        return;
    }

    // Buttons and keys that are logically down must be released before we stop
    // emulating: eis_device_pause() would reset the state silently, and a client
    // that never sees the releases ends up with stuck keys.
    this->release_held_input();

    for (auto *dev : {this->pointer, this->absolute, this->keyboard})
    {
        if (dev)
        {
            eis_device_stop_emulating(dev);
        }
    }

    if (this->cursor_hidden)
    {
        wf::get_core().unhide_cursor();
        this->cursor_hidden = false;
    }

    LOGI("eis: deactivated (activation_id=", this->activation_id, ")");

    if (emit_signal)
    {
        this->emit_deactivated();
    }

    this->have_last = false;
}

void eis_session_t::release_held_input()
{
    if (!this->keys_pressed.empty() && this->keyboard)
    {
        for (auto kc : this->keys_pressed)
        {
            eis_device_keyboard_key(this->keyboard, kc, false);
        }

        eis_device_frame(this->keyboard, eis_now(this->context));
        this->keys_pressed.clear();
    }

    if (!this->buttons_pressed.empty() && this->pointer)
    {
        for (auto b : this->buttons_pressed)
        {
            eis_device_button_button(this->pointer, b, false);
        }

        eis_device_frame(this->pointer, eis_now(this->context));
        this->buttons_pressed.clear();
    }

    if (this->have_modifiers && this->keyboard)
    {
        this->have_modifiers = false;
        eis_device_keyboard_send_xkb_modifiers(this->keyboard, 0, 0, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Input path
// ---------------------------------------------------------------------------

uint32_t eis_session_t::find_crossed_barrier(double px, double py, double cx,
    double cy, double *ix, double *iy) const
{
    // wlroots has already constrained the cursor to the output layout by the
    // time this runs, so the cursor stops *on* the barrier rather than passing
    // through it. Two tolerances are needed:
    //
    //  - REACH_TOL: the layout box is expressed in wl_fixed and the clamp lands
    //    one unit (1/256) shy of the exact boundary -- 719.99609375 for a
    //    720-tall output -- so without slack the barrier is never quite reached.
    //  - SPAN_TOL: leaving through a corner leaves the cursor a hair past the
    //    end of the barrier's along-axis span (1279.996 vs a span ending 1279).
    constexpr double REACH_TOL = 0.05;
    constexpr double SPAN_TOL = 1.0;

    for (const auto& [id, b] : this->barriers)
    {
        const bool vertical = (b.ox != 0);

        const double line = vertical ? b.x1 : b.y1;
        const double before = (vertical ? px : py) - line;
        const double after = (vertical ? cx : cy) - line;
        const int outward = vertical ? b.ox : b.oy;

        const double a1 = vertical ? b.y1 : b.x1;
        const double a2 = vertical ? b.y2 : b.x2;
        const double lo = std::min(a1, a2);
        const double hi = std::max(a1, a2);
        const double along = vertical ? cy : cx;

        const bool within = (along >= lo - SPAN_TOL) && (along <= hi + SPAN_TOL);

        // Strictly inside before, and now reached-or-past while the movement is
        // still pushing outwards.
        const bool crossed = (before * outward < 0)
            && (after * outward >= -REACH_TOL);

        if (within && crossed)
        {
            const double at = std::min(std::max(along, lo), hi);
            if (vertical)
            {
                *ix = line;
                *iy = at;
            }
            else
            {
                *ix = at;
                *iy = line;
            }

            return id;
        }
    }

    return 0;
}

bool eis_session_t::handle_motion(double dx, double dy, double cx, double cy)
{
    // While the compositor is warping the cursor back onto a barrier we have just
    // crossed, ignore the motion event our own warp generates.
    if (this->warping)
    {
        return false;
    }

    if (!this->have_last)
    {
        this->last_x = cx;
        this->last_y = cy;
        this->have_last = true;
    }

    double px = this->last_x;
    double py = this->last_y;


    if (this->session_state == session_state_t::ACTIVATED)
    {
        this->emit_motion(dx, dy);
        this->emit_motion_absolute(cx, cy);
        this->last_x = cx;
        this->last_y = cy;
        return true;
    }

    if (this->session_state != session_state_t::ENABLED)
    {
        this->last_x = cx;
        this->last_y = cy;
        return false;
    }

    // Enabled but not yet capturing: watch for a barrier crossing.
    double ix = 0, iy = 0;
    uint32_t barrier_id = this->find_crossed_barrier(px, py, cx, cy, &ix, &iy);

    if (!barrier_id)
    {
        this->last_x = cx;
        this->last_y = cy;
        return false;
    }

    this->activate(barrier_id, cx, cy);

    // mutter sends the *unconstrained remainder* of the motion -- the part the
    // barrier prevented from being applied -- so the remote cursor keeps moving
    // "into" the edge at wall-clock speed and overshoots, while the local cursor
    // stays put. Deskflow depends on that overshoot to work out where to place
    // its cursor when capture is released.
    //
    // Here that is computable exactly: dx/dy are the raw deltas, and
    // (cx - px) / (cy - py) is what wlroots actually let through after
    // constraining the cursor to the layout.
    const double clamped_dx = cx - px;
    const double clamped_dy = cy - py;
    this->emit_motion(dx - clamped_dx, dy - clamped_dy);
    this->emit_motion_absolute(cx, cy);

    // Park the cursor on the barrier so the desktop does not creep.
    this->warping = true;
    auto *cursor = wf::get_core().get_wlr_cursor();
    for (auto dev : wf::get_core().get_input_devices())
    {
        auto *handle = dev->get_wlr_handle();
        if (handle && handle->type == WLR_INPUT_DEVICE_POINTER)
        {
            wlr_cursor_warp(cursor, handle, ix, iy);
            break;
        }
    }

    this->warping = false;
    this->last_x = ix;
    this->last_y = iy;
    return true;
}

void eis_session_t::note_button(uint32_t button, bool pressed)
{
    if (pressed)
    {
        this->buttons_pressed.insert(button);
    }
    else
    {
        this->buttons_pressed.erase(button);
    }
}

void eis_session_t::note_key(uint32_t keycode, bool pressed)
{
    if (pressed)
    {
        this->keys_pressed.insert(keycode);
    }
    else
    {
        this->keys_pressed.erase(keycode);
    }
}

void eis_session_t::send_modifiers(uint32_t depressed, uint32_t latched,
    uint32_t locked, uint32_t group)
{
    this->mods_depressed = depressed;
    this->mods_latched = latched;
    this->mods_locked = locked;
    this->mods_group = group;
    this->have_modifiers = true;

    if (this->session_state != session_state_t::ACTIVATED || !this->keyboard)
    {
        return;
    }

    // C argument order is (depressed, latched, locked, group); libei remaps it to
    // the wire order (depressed, locked, latched, group). This is a state
    // announcement rather than an input event, so it needs no frame.
    eis_device_keyboard_send_xkb_modifiers(this->keyboard, depressed, latched,
        locked, group);
}

void eis_session_t::emit_motion(double dx, double dy)
{
    if (!this->pointer)
    {
        return;
    }

    eis_device_pointer_motion(this->pointer, dx, dy);

    // Without a frame the event stays in the device's pending queue forever and
    // the client sees nothing at all.
    eis_device_frame(this->pointer, eis_now(this->context));
}

void eis_session_t::emit_motion_absolute(double x, double y)
{
    if (!this->absolute)
    {
        return;
    }

    // Coordinates are logical pixels inside one of the device's regions.
    eis_device_pointer_motion_absolute(this->absolute, x, y);
    eis_device_frame(this->absolute, eis_now(this->context));
}

void eis_session_t::emit_button(uint32_t button_code, bool pressed)
{
    if (!this->pointer)
    {
        return;
    }

    // button_code must be a linux/input-event-codes BTN_* value; libei rejects
    // anything below BTN_MOUSE (0x110).
    eis_device_button_button(this->pointer, button_code, pressed);
    eis_device_frame(this->pointer, eis_now(this->context));
}

void eis_session_t::emit_scroll(double horizontal, double vertical)
{
    if (!this->pointer)
    {
        return;
    }

    // NOTE the argument order: eis_device_scroll_delta() is (x = horizontal,
    // y = vertical), even though the local variable order reads the other way.
    if (horizontal != 0.0)
    {
        eis_device_scroll_delta(this->pointer, horizontal, 0.0);
    }

    if (vertical != 0.0)
    {
        eis_device_scroll_delta(this->pointer, 0.0, vertical);
    }

    eis_device_frame(this->pointer, eis_now(this->context));
}

void eis_session_t::emit_key(uint32_t keycode, bool pressed)
{
    if (!this->keyboard)
    {
        return;
    }

    // keycode must be a linux/input-event-codes value.
    eis_device_keyboard_key(this->keyboard, keycode, pressed);
    eis_device_frame(this->keyboard, eis_now(this->context));
}

// ---------------------------------------------------------------------------
// D-Bus signals
// ---------------------------------------------------------------------------

void eis_session_t::emit_signal(const char *member, const char *signature, ...)
{
    if (!this->bus)
    {
        return;
    }

    sd_bus_message *msg = nullptr;
    int r = sd_bus_message_new_signal(this->bus, &msg, this->object_path.c_str(),
        DBUS_SESSION_IFACE, member);
    if (r < 0)
    {
        return;
    }

    va_list ap;
    va_start(ap, signature);
    r = sd_bus_message_appendv(msg, signature, ap);
    va_end(ap);

    if (r >= 0)
    {
        sd_bus_send(nullptr, msg, nullptr);
    }

    sd_bus_message_unref(msg);
}

void eis_session_t::emit_activated(uint32_t barrier_id, double cx, double cy)
{
    this->emit_signal("Activated", "uudd", this->barriers.count(barrier_id)
        ? barrier_id : 0u, this->activation_id, cx, cy);
}

void eis_session_t::emit_deactivated()
{
    this->emit_signal("Deactivated", "u", this->activation_id);
}

void eis_session_t::emit_zones_changed()
{
    this->emit_signal("ZonesChanged", "");
}

void eis_session_t::emit_disabled()
{
    this->emit_signal("Disabled", "");
}

// ---------------------------------------------------------------------------
// EI plumbing (unchanged from M2)
// ---------------------------------------------------------------------------

void eis_session_t::on_eis_readable()
{
    // Dispatching does not necessarily queue events; call it immediately once
    // data is available.
    eis_dispatch(this->context);

    while (true)
    {
        // eis_get_event() returns a new reference; every event must be released,
        // including ones we do not handle.
        struct eis_event *event = eis_get_event(this->context);
        if (!event)
        {
            break;
        }

        this->handle_eis_event(event);
        eis_event_unref(event);
    }
}

void eis_session_t::handle_eis_event(struct eis_event *event)
{
    switch (eis_event_get_type(event))
    {
      case EIS_EVENT_CLIENT_CONNECT:
      {
        auto *c = eis_event_get_client(event);

        // An input *capture* client consumes events from us. A sender-mode
        // client would be feeding us input, which is the RemoteDesktop
        // direction and not ours to serve.
        if (eis_client_is_sender(c))
        {
            LOGW("eis: unexpected sender-mode client connected, disconnecting");
            eis_client_disconnect(c);
            return;
        }

        if (this->client)
        {
            LOGW("eis: unexpected second client connected, disconnecting");
            eis_client_disconnect(c);
            return;
        }

        this->setup_client(c);
        break;
      }

      case EIS_EVENT_CLIENT_DISCONNECT:
      {
        LOGI("eis: client disconnected");

        // libei has already removed and destroyed our devices and seat, but we
        // still hold our own references.
        this->pointer = nullptr;
        this->absolute = nullptr;
        this->keyboard = nullptr;

        if (this->seat)
        {
            eis_seat_unref(this->seat);
            this->seat = nullptr;
        }

        this->client = nullptr;
        this->bound_caps = 0;
        this->session_state = session_state_t::INIT;
        this->have_last = false;
        this->keys_pressed.clear();
        this->buttons_pressed.clear();
        break;
      }

      case EIS_EVENT_SEAT_BIND:
      {
        // This is the *complete* bound set as of this event, not a delta.
        this->bound_caps = 0;
        for (auto cap : {EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_KEYBOARD})
        {
            if (eis_event_seat_has_capability(event, cap))
            {
                this->bound_caps |= cap;
            }
        }

        this->ensure_eis_devices();
        break;
      }

      case EIS_EVENT_SEAT_DEVICE_REQUESTED:
        this->ensure_eis_devices();
        break;

      case EIS_EVENT_DEVICE_READY:
      {
        // The client finished configuring the device; only now may we resume it.
        auto *dev = eis_event_get_device(event);
        eis_device_resume(dev);

        // Normally emulation is gated on the barrier crossing, and devices that
        // arrive before activation are simply left resumed. But a device can also
        // appear while the session is already active (client reconnects late, or
        // binds capabilities after activation), and then it must be armed
        // immediately or it will silently never deliver events.
        if (this->session_state == session_state_t::ACTIVATED)
        {
            eis_device_start_emulating(dev, this->activation_id);
        }

        break;
      }

      case EIS_EVENT_DEVICE_CLOSED:
      {
        auto *dev = eis_event_get_device(event);
        LOGI("eis: client closed a device");

        /*
         * libei has already removed and destroyed this device by the time it
         * reports DEVICE_CLOSED, exactly as for CLIENT_DISCONNECT below. Calling
         * eis_device_remove() on it touches freed memory, and leaving our own
         * pointer set means the next ensure_eis_devices() frees it a second time.
         * So just drop the pointer; deliberately do not unref, since releasing a
         * reference the library has already torn down is the same hazard.
         */
        if (dev == this->pointer)
        {
            this->pointer = nullptr;
        }

        if (dev == this->absolute)
        {
            this->absolute = nullptr;
        }

        if (dev == this->keyboard)
        {
            this->keyboard = nullptr;
        }

        break;
      }

      default:
        // Unknown events are legal; libei may add types. Never abort here.
        break;
    }
}

void eis_session_t::setup_client(eis_client *c)
{
    this->client = c;

    // The seat must exist and be configured before devices are created on it:
    // eis_device_configure_capability() silently drops capabilities the seat does
    // not advertise.
    this->seat = eis_client_new_seat(c, "seat0");
    if (!this->seat)
    {
        LOGE("eis: eis_client_new_seat() failed");
        eis_client_disconnect(c);
        this->client = nullptr;
        return;
    }

    for (auto cap : {EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_POINTER_ABSOLUTE,
             EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL, EIS_DEVICE_CAP_KEYBOARD})
    {
        eis_seat_configure_capability(this->seat, cap);
    }

    // Only now is the seat announced to the client. Devices may only be created
    // after the client has bound to it (EIS_EVENT_SEAT_BIND).
    eis_seat_add(this->seat);
    eis_client_connect(c);

    LOGI("eis: client connected, seat announced");
}

void eis_session_t::ensure_eis_devices()
{
    if (!this->seat)
    {
        return;
    }

    const bool wants_pointer = (this->bound_caps & EIS_DEVICE_CAP_POINTER);
    const bool wants_keyboard = (this->bound_caps & EIS_DEVICE_CAP_KEYBOARD);

    if (wants_pointer)
    {
        if (!this->pointer)
        {
            this->pointer = this->create_pointer_device();
        }

        if (!this->absolute)
        {
            this->absolute = this->create_absolute_device();
        }
    }
    else
    {
        this->destroy_device(this->pointer);
        this->pointer = nullptr;
        this->destroy_device(this->absolute);
        this->absolute = nullptr;
    }

    if (wants_keyboard && !this->keyboard)
    {
        this->keyboard = this->create_keyboard_device();
    }
    else if (!wants_keyboard)
    {
        this->destroy_device(this->keyboard);
        this->keyboard = nullptr;
    }
}

void eis_session_t::add_output_regions(eis_device *device)
{
    // Regions are mandatory on a VIRTUAL device advertising POINTER_ABSOLUTE,
    // and they are how the client learns the desktop layout. One region per
    // zone, in logical layout coordinates.
    for (const auto &z : this->zone_list)
    {
        struct eis_region *region = eis_device_new_region(device);
        if (!region)
        {
            LOGE("eis: eis_device_new_region() failed");
            continue;
        }

        eis_region_set_offset(region, z.x, z.y);
        eis_region_set_size(region, z.width, z.height);

        // A region must have a size to be valid; all setters must precede the
        // add, and the add takes its own reference, so drop ours afterwards.
        eis_region_add(region);
        eis_region_unref(region);
    }
}

eis_device *eis_session_t::create_pointer_device()
{
    auto *dev = eis_seat_new_device(this->seat);
    if (!dev)
    {
        return nullptr;
    }

    // The device type must be configured first: it gates what else is legal, and
    // eis_device_new_region() reads it immediately.
    eis_device_configure_type(dev, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(dev, "wayfire-pointer");
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_POINTER);
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_BUTTON);
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_SCROLL);
    this->add_output_regions(dev);

    eis_device_add(dev);
    return dev;
}

eis_device *eis_session_t::create_absolute_device()
{
    auto *dev = eis_seat_new_device(this->seat);
    if (!dev)
    {
        return nullptr;
    }

    eis_device_configure_type(dev, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(dev, "wayfire-pointer-absolute");
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_BUTTON);
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_SCROLL);
    this->add_output_regions(dev);

    eis_device_add(dev);
    return dev;
}

eis_device *eis_session_t::create_keyboard_device()
{
    auto *dev = eis_seat_new_device(this->seat);
    if (!dev)
    {
        return nullptr;
    }

    eis_device_configure_type(dev, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(dev, "wayfire-keyboard");
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_KEYBOARD);

    // Ship the compositor's own xkb keymap. wlroots keeps it in a sealed memfd
    // and exposes exactly the fd+size pair that eis_device_new_keymap() wants.
    // libei dup()s the fd, so we do not need to hold it open.
    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(
        wf::get_core().get_current_seat());
    if (kbd && kbd->keymap_fd >= 0 && kbd->keymap_size > 0)
    {
        auto *km = eis_device_new_keymap(dev, EIS_KEYMAP_TYPE_XKB,
            kbd->keymap_fd, kbd->keymap_size);
        if (km)
        {
            // Must precede eis_device_add(), and may only be applied once.
            eis_keymap_add(km);
            eis_keymap_unref(km);
            LOGI("eis: attached keymap, ", kbd->keymap_size, " bytes");
        }
        else
        {
            LOGW("eis: eis_device_new_keymap() failed; client will get no keymap");
        }
    }
    else
    {
        LOGW("eis: no keyboard/keymap available; client will get no keymap");
    }

    eis_device_add(dev);
    return dev;
}

void eis_session_t::destroy_device(eis_device *dev)
{
    if (!dev)
    {
        return;
    }

    eis_device_remove(dev);
    eis_device_unref(dev);
}

// ---------------------------------------------------------------------------
// eis_server_t
// ---------------------------------------------------------------------------

void eis_server_t::init()
{
    auto loop = wf::get_core().ev_loop;

    int r = sd_bus_open_user(&this->bus);
    if (r < 0)
    {
        LOGE("eis: sd_bus_open_user() failed: ", errstr(-r));
        this->bus = nullptr;
        return;
    }

    this->bus_read_source = wl_event_loop_add_fd(loop, sd_bus_get_fd(this->bus),
        WL_EVENT_READABLE, [](int fd, uint32_t mask, void *data) -> int
    {
        static_cast<eis_server_t*>(data)->on_bus_readable(fd, mask);
        return 0;
    }, this);

    r = sd_bus_add_object_vtable(this->bus, &this->manager_slot, DBUS_MANAGER_PATH,
        DBUS_MANAGER_IFACE, manager_vtable, this);
    if (r < 0)
    {
        LOGE("eis: failed to export the manager object: ", errstr(-r));
    }

    r = sd_bus_request_name(this->bus, DBUS_SERVICE, 0);
    if (r < 0)
    {
        LOGE("eis: failed to acquire ", DBUS_SERVICE, ": ", errstr(-r));
    }
    else
    {
        LOGI("eis: compositor-side emulated-input server ready on ", DBUS_SERVICE);
    }
}

void eis_server_t::fini()
{
    if (this->bus)
    {
        for (auto& [path, session] : this->sessions)
        {
            if (session->session_slot)
            {
                sd_bus_slot_unref(session->session_slot);
                session->session_slot = nullptr;
            }

        }

        this->sessions.clear();


        if (this->bus_read_source)
        {
            wl_event_source_remove(this->bus_read_source);
            this->bus_read_source = nullptr;
        }

        if (this->bus_write_source)
        {
            wl_event_source_remove(this->bus_write_source);
            this->bus_write_source = nullptr;
        }

        if (this->manager_slot)
        {
            sd_bus_slot_unref(this->manager_slot);
            this->manager_slot = nullptr;
        }

        sd_bus_flush(this->bus);
        sd_bus_unref(this->bus);
        this->bus = nullptr;
    }
}

void eis_server_t::on_bus_readable(int fd, uint32_t mask)
{
    int r = sd_bus_process(this->bus, nullptr);


    if (r < 0)
    {
        LOGE("eis: sd_bus_process() failed: ", errstr(-r));
        return;
    }

    this->update_bus_sources();
}

void eis_server_t::on_bus_writable(int fd, uint32_t mask)
{
    sd_bus_flush(this->bus);
    this->update_bus_sources();
}

void eis_server_t::update_bus_sources()
{
    if (!this->bus)
    {
        return;
    }

    bool ready = sd_bus_is_ready(this->bus) > 0;

    if (ready && this->bus_write_source)
    {
        wl_event_source_remove(this->bus_write_source);
        this->bus_write_source = nullptr;
    }
    else if (!ready && !this->bus_write_source)
    {
        this->bus_write_source = wl_event_loop_add_fd(wf::get_core().ev_loop,
            sd_bus_get_fd(this->bus), WL_EVENT_WRITABLE,
            [](int fd, uint32_t mask, void *data) -> int
        {
            static_cast<eis_server_t*>(data)->on_bus_writable(fd, mask);
            return 0;
        }, this);
    }
}

eis_session_ptr eis_server_t::create_session(uint32_t capabilities,
    const std::string &peer)
{
    if (!this->bus)
    {
        return nullptr;
    }

    uint32_t granted = capabilities & EIS_SUPPORTED_CAPABILITIES;
    if (granted == 0)
    {
        LOGW("eis: requested capabilities ", hex(capabilities),
            " are not supported");
        return nullptr;
    }

    auto loop = wf::get_core().ev_loop;

    uint32_t session_id = this->next_session_id++;
    auto path = std::string(DBUS_MANAGER_PATH) + "/u" + std::to_string(session_id);
    auto session = std::make_shared<eis_session_t>(this, path, granted, peer,
        session_id);
    session->bus = this->bus;

    int r = sd_bus_add_object_vtable(this->bus, &session->session_slot,
        path.c_str(), DBUS_SESSION_IFACE, session_vtable, session.get());
    if (r < 0)
    {
        LOGE("eis: failed to export session object ", path, ": ", errstr(-r));
        return nullptr;
    }

    this->sessions[path] = session;

    // Each session owns its own EIS server context, so that EIS clients reaching
    // it are attributable to this session by construction.
    session->context = eis_new(session.get());
    if (!session->context)
    {
        LOGE("eis: eis_new() failed");
    }
    else
    {
        eis_log_set_handler(session->context, eis_log_trampoline);
        eis_log_set_priority(session->context, EIS_LOG_PRIORITY_DEBUG);

        // Must precede backend setup; libei returns -EBUSY afterwards.
        if (eis_set_flag(session->context, EIS_FLAG_DEVICE_READY) < 0)
        {
            LOGE("eis: eis_set_flag(EIS_FLAG_DEVICE_READY) failed");
        }
        else if (eis_setup_backend_fd(session->context) < 0)
        {
            LOGE("eis: failed to set up the EIS backend");
        }
        else
        {
            // eis_get_fd() is an epoll fd, level-triggered; requesting
            // WL_EVENT_WRITABLE would busy-loop.
            session->eis_source = wl_event_loop_add_fd(loop,
                eis_get_fd(session->context), WL_EVENT_READABLE,
                [](int fd, uint32_t mask, void *data) -> int
            {
                static_cast<eis_session_t*>(data)->on_eis_readable();
                return 0;
            }, session.get());
        }
    }

    LOGI("eis: created session ", path, " with capabilities ", hex(granted));
    return session;
}

eis_session_ptr eis_server_t::enabled_session()
{
    for (auto& [path, session] : this->sessions)
    {
        if (session->state() != session_state_t::INIT)
        {
            return session;
        }
    }

    return nullptr;
}

eis_session_ptr eis_server_t::active_session()
{
    for (auto& [path, session] : this->sessions)
    {
        if (session->state() == session_state_t::ACTIVATED)
        {
            return session;
        }
    }

    return nullptr;
}

void eis_server_t::zones_changed()
{
    for (auto& [path, session] : this->sessions)
    {
        session->refresh_zones();
    }
}

void eis_server_t::session_closed(const eis_session_ptr &session)
{
    this->sessions.erase(session->path());

    if (session->session_slot)
    {
        sd_bus_slot_unref(session->session_slot);
        session->session_slot = nullptr;
    }


    LOGI("eis: session ", session->path(), " closed");
}

// ---------------------------------------------------------------------------
// D-Bus handlers
// ---------------------------------------------------------------------------

namespace
{
int handle_get_supported_capabilities(sd_bus *bus, const char *object,
    const char *iface, const char *property, sd_bus_message *msg, void *userdata,
    sd_bus_error *error)
{
    // In sd-bus a property getter appends the value straight to the incoming
    // message and returns the number of bytes appended.
    return sd_bus_message_append(msg, "u", (uint32_t)EIS_SUPPORTED_CAPABILITIES);
}

int handle_create_session(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
    auto *server = static_cast<eis_server_t*>(userdata);

    uint32_t capabilities = 0;
    int r = sd_bus_message_read(msg, "u", &capabilities);
    if (r < 0)
    {
        sd_bus_error_set_const(ret_error, SD_BUS_ERROR_INVALID_ARGS,
            "Expected a capabilities bitmask");
        return -EINVAL;
    }

    const char *peer = sd_bus_message_get_sender(msg);
    auto session = server->create_session(capabilities, peer ? peer : "");
    if (!session)
    {
        // Must return here: falling through would dereference the null session.
        sd_bus_error_set_const(ret_error, SD_BUS_ERROR_NOT_SUPPORTED,
            "Requested capabilities are not supported");
        return -ENOTSUP;
    }

    return sd_bus_reply_method_return(msg, "o", session->path().c_str());
}

namespace
{
/**
 * Send an empty method reply and tell sd-bus we did.
 *
 * sd_bus_reply_method_return(msg, "") looks like the obvious way to answer a
 * void method, but it returns the number of bytes appended -- zero for an empty
 * reply -- and a handler that returns 0 leaves sd-bus to synthesise the reply,
 * which this sd-bus build does not do. The caller then sees no reply at all
 * ("Message recipient disconnected ... NoReply"). So send the reply directly and
 * return a positive value.
 */
int reply_empty(sd_bus_message *msg)
{
    sd_bus_message *reply = nullptr;
    int r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0)
    {
        return r;
    }

    r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r < 0 ? r : 1;
}
} // namespace

/** Shared permission gate for every session-scoped method. */
#define EIS_REQUIRE_SESSION(msg, ret_error) \
    auto *session = static_cast<eis_session_t*>(userdata); \
    if (!session->check_permission(msg)) \
    { \
        /* Return a negative errno: a positive return from a method handler \
         * means "reply already sent", so the caller would block instead of \
         * seeing the error. */ \
        sd_bus_error_set_const(ret_error, SD_BUS_ERROR_ACCESS_DENIED, \
            "Caller does not own this session"); \
        return -EACCES; \
    }

int handle_get_zones(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
    EIS_REQUIRE_SESSION(msg, ret_error)

    // The output layout may not have been computed when the session was created,
    // so re-derive the zones on every query. This is a no-op unless the layout
    // actually changed, in which case the serial is bumped and ZonesChanged is
    // emitted as the spec requires.
    session->refresh_zones();

    sd_bus_message *reply = nullptr;
    int r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0)
    {
        return r;
    }

    // Declared reply signature is "ua(uuii)". sd-bus treats a multi-argument
    // method reply as an implicit struct: the members are appended *flat*, not
    // wrapped in an explicit struct container. Opening an 'r' here makes sd-bus
    // reject the reply and the caller sees ENOMSG.
    r = sd_bus_message_append(reply, "u", session->serial());
    if (r < 0)
    {
        goto fail;
    }

    r = sd_bus_message_open_container(reply, 'a', "(uuii)");
    if (r < 0)
    {
        goto fail;
    }

    for (const auto &z : session->zones())
    {
        r = sd_bus_message_append(reply, "(uuii)", z.width, z.height, z.x, z.y);
        if (r < 0)
        {
            goto fail;
        }
    }

    r = sd_bus_message_close_container(reply);
    if (r < 0)
    {
        goto fail;
    }

    r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return (r < 0) ? r : 1;

fail:
    sd_bus_message_unref(reply);
    return r;
}

int handle_add_barrier(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
    EIS_REQUIRE_SESSION(msg, ret_error)

    uint32_t serial = 0;
    int32_t x1, y1, x2, y2;
    int r = sd_bus_message_read(msg, "u(iiii)", &serial, &x1, &y1, &x2, &y2);
    if (r < 0)
    {
        sd_bus_error_set_const(ret_error, SD_BUS_ERROR_INVALID_ARGS,
            "Expected (u serial, (iiii) position)");
        return -EINVAL;
    }

    uint32_t id = session->add_barrier(serial, x1, y1, x2, y2);
    return sd_bus_reply_method_return(msg, "u", id);
}

int handle_clear_barriers(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
    EIS_REQUIRE_SESSION(msg, ret_error)

    session->clear_barriers();

    // Send the empty reply explicitly: returning 0 would leave sd-bus to
    // synthesise it, which this sd-bus build does not do.
    return reply_empty(msg);
}

int handle_enable(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
    EIS_REQUIRE_SESSION(msg, ret_error)

    session->enable();

    // Send the empty reply explicitly: returning 0 would leave sd-bus to
    // synthesise it, which this sd-bus build does not do.
    return reply_empty(msg);
}

int handle_disable(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
    EIS_REQUIRE_SESSION(msg, ret_error)

    session->disable();

    // Send the empty reply explicitly: returning 0 would leave sd-bus to
    // synthesise it, which this sd-bus build does not do.
    return reply_empty(msg);
}

int handle_release(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
    EIS_REQUIRE_SESSION(msg, ret_error)

    uint32_t activation_id = 0;
    int r = sd_bus_message_enter_container(msg, 'a', "{sv}");
    if (r >= 0)
    {
        while ((r = sd_bus_message_enter_container(msg, 'e', "sv")) > 0)
        {
            const char *key = nullptr;
            sd_bus_message_read(msg, "s", &key);
            if (key && !strcmp(key, "activation_id"))
            {
                sd_bus_message_read(msg, "v", "u", &activation_id);
            }

            sd_bus_message_exit_container(msg);
        }

        sd_bus_message_exit_container(msg);
    }

    session->release(activation_id);

    // Send the empty reply explicitly; returning 0 does not work here.
    return reply_empty(msg);
}

int handle_connect_to_eis(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
    EIS_REQUIRE_SESSION(msg, ret_error)

    int fd = eis_backend_fd_add_client(session->context);
    if (fd < 0)
    {
        return sd_bus_error_set_errno(ret_error, -fd);
    }

    sd_bus_message *reply = nullptr;
    int r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0)
    {
        close(fd);
        return r;
    }

    // The fd in the message is owned by the message; hold our copy open until
    // after the send so that we do not depend on sd-bus duping it.
    r = sd_bus_message_append(reply, "h", fd);
    close(fd);
    if (r < 0)
    {
        sd_bus_message_unref(reply);
        return r;
    }

    r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return (r < 0) ? r : 1;
}

int handle_session_close(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
    EIS_REQUIRE_SESSION(msg, ret_error)

    session->emit_signal("Closed", "");
    session->close();
    session->server()->session_closed(session->shared_from_this());

    return reply_empty(msg);
}
} // namespace

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

namespace
{
/**
 * The Wayfire input taps that feed a session's EIS devices.
 *
 * These have to live in the compositor rather than the portal because only the
 * compositor can (a) observe the pointer position before it is applied, and
 * (b) withhold events from the normal pipeline while capture is active.
 *
 * wf::input_event_signal is the right hook: Wayfire documents it as emitted
 * "before any processing is done", and wlroots has already updated wlr_cursor's
 * x/y by the time it fires, so get_cursor_position() returns the new,
 * unconstrained position -- which is what barrier hit-testing needs.
 *
 * Note that only input_event_processing_mode_t::IGNORE suppresses pointer
 * motion. NO_CLIENT is honoured for pointer_button / keyboard_key / touch_down
 * only, because pointer_t::handle_pointer_motion() ignores its mode parameter.
 */
class WayfireEisPlugin : public wf::plugin_interface_t
{
  public:
    void init() override
    {
        this->server.init();

        auto &core = wf::get_core();
        core.connect(&this->on_pointer_motion);
        core.connect(&this->on_pointer_button);
        core.connect(&this->on_pointer_axis);
        core.connect(&this->on_keyboard_key);
        core.connect(&this->on_keyboard_modifiers);

        // Zone changes invalidate barriers, so every session must be told.
        core.output_layout->connect(&this->on_output_added);
        core.output_layout->connect(&this->on_output_removed);
        core.output_layout->connect(&this->on_layout_changed);
    }

    void fini() override
    {
        auto &core = wf::get_core();
        core.disconnect(&this->on_pointer_motion);
        core.disconnect(&this->on_pointer_button);
        core.disconnect(&this->on_pointer_axis);
        core.disconnect(&this->on_keyboard_key);
        core.disconnect(&this->on_keyboard_modifiers);

        core.output_layout->disconnect(&this->on_output_added);
        core.output_layout->disconnect(&this->on_output_removed);
        core.output_layout->disconnect(&this->on_layout_changed);

        this->server.fini();
    }

  private:
    eis_server_t server;

    /** The session that is capturing right now (for divert-only taps). */
    eis_session_ptr active()
    {
        return this->server.active_session();
    }

    /**
     * The session that should see pointer motion. This must include sessions
     * that are merely ENABLED, because detecting the barrier crossing is what
     * promotes them to ACTIVATED.
     */
    eis_session_ptr motion_target()
    {
        return this->server.enabled_session();
    }

    wf::signal::connection_t<wf::input_event_signal<wlr_pointer_motion_event>>
    on_pointer_motion = [=] (wf::input_event_signal<wlr_pointer_motion_event> *ev)
    {
        auto s = this->motion_target();
        if (!s)
        {
            return;
        }

        auto pos = wf::get_core().get_cursor_position();
        if (std::isnan(pos.x) || std::isnan(pos.y))
        {
            return;
        }

        if (s->handle_motion(ev->event->delta_x, ev->event->delta_y, pos.x, pos.y))
        {
            // Withhold the event from the rest of the compositor: this is what
            // makes capture exclusive rather than a duplicate feed.
            ev->mode = wf::input_event_processing_mode_t::IGNORE;
        }
    };

    wf::signal::connection_t<wf::input_event_signal<wlr_pointer_button_event>>
    on_pointer_button = [=] (wf::input_event_signal<wlr_pointer_button_event> *ev)
    {
        auto s = this->active();
        if (!s)
        {
            return;
        }

        const bool pressed =
            ev->event->state == WL_POINTER_BUTTON_STATE_PRESSED;
        s->note_button(ev->event->button, pressed);
        s->emit_button(ev->event->button, pressed);
        ev->mode = wf::input_event_processing_mode_t::IGNORE;
    };

    wf::signal::connection_t<wf::input_event_signal<wlr_pointer_axis_event>>
    on_pointer_axis = [=] (wf::input_event_signal<wlr_pointer_axis_event> *ev)
    {
        auto s = this->active();
        if (!s)
        {
            return;
        }

        // Wayfire delivers one event per axis with an already-accumulated delta.
        // Note that wlr_pointer_axis_event::delta is a plain double in logical
        // pixels, *not* a wl_fixed.
        const double delta = ev->event->delta;

        if (ev->event->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        {
            s->emit_scroll(delta, 0.0);
        }
        else
        {
            s->emit_scroll(0.0, delta);
        }

        ev->mode = wf::input_event_processing_mode_t::IGNORE;
    };

    wf::signal::connection_t<wf::input_event_signal<wlr_keyboard_key_event>>
    on_keyboard_key = [=] (wf::input_event_signal<wlr_keyboard_key_event> *ev)
    {
        auto s = this->active();
        if (!s)
        {
            return;
        }

        // ::state is the press/release state; ::update_state is only a bool
        // meaning "the backend does not update modifiers on its own".
        const bool pressed =
            ev->event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
        s->note_key(ev->event->keycode, pressed);
        s->emit_key(ev->event->keycode, pressed);
        ev->mode = wf::input_event_processing_mode_t::IGNORE;
    };

    wf::signal::connection_t<wf::input_event_signal<mwlr_keyboard_modifiers_event>>
    on_keyboard_modifiers =
        [=] (wf::input_event_signal<mwlr_keyboard_modifiers_event> *ev)
    {
        auto s = this->active();
        if (!s)
        {
            return;
        }

        auto *kbd = wlr_seat_get_keyboard(wf::get_core().get_current_seat());
        s->send_modifiers(kbd->modifiers.depressed, kbd->modifiers.latched,
            kbd->modifiers.locked, kbd->modifiers.group);
    };

    wf::signal::connection_t<wf::output_added_signal>
    on_output_added = [=] (wf::output_added_signal *)
    {
        this->server.zones_changed();
    };

    wf::signal::connection_t<wf::output_removed_signal>
    on_output_removed = [=] (wf::output_removed_signal *)
    {
        this->server.zones_changed();
    };

    wf::signal::connection_t<wf::output_layout_configuration_changed_signal>
    on_layout_changed = [=] (wf::output_layout_configuration_changed_signal *)
    {
        this->server.zones_changed();
    };
};
} // namespace

DECLARE_WAYFIRE_PLUGIN(WayfireEisPlugin)