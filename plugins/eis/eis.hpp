#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Opaque libei types.
struct eis;
struct eis_client;
struct eis_device;
struct eis_event;
struct eis_seat;

// Opaque systemd / wayland types.
struct sd_bus;
struct sd_bus_message;
struct sd_bus_slot;
struct sd_bus_vtable;
struct wl_event_source;

/**
 * Capability bits. These mirror the XDG InputCapture portal's
 * SupportedCapabilities / capabilities values so that the portal layer does not
 * have to translate.
 */
enum
{
    EIS_CAP_KEYBOARD = 1,
    EIS_CAP_POINTER  = 2,
    EIS_CAP_TOUCH    = 4,
};

/** Capabilities this compositor knows how to capture. */
constexpr uint32_t EIS_SUPPORTED_CAPABILITIES = EIS_CAP_KEYBOARD | EIS_CAP_POINTER;

class eis_server_t;
class eis_session_t;
using eis_session_ptr = std::shared_ptr<eis_session_t>;

/** A logical screen area available to a session, in logical layout pixels. */
struct zone_t
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    bool operator==(const zone_t& o) const
    {
        return this->x == o.x && this->y == o.y && this->width == o.width
               && this->height == o.height;
    }

    bool operator!=(const zone_t& o) const
    {
        return !(*this == o);
    }
};

/**
 * A pointer barrier: a horizontal or vertical line at the edge of a zone.
 *
 * ox/oy record the outward normal (+1/-1), i.e. which side of the line is
 * *outside* the desktop. wlroots constrains the cursor to the output layout
 * before our input tap runs, so a barrier crossing can never be observed as a
 * geometric crossing: the cursor comes to rest exactly on the line. We instead
 * detect "reached the line while still being pushed outward".
 */
struct barrier_t
{
    uint32_t id = 0;
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
    int ox = 0;
    int oy = 0;
};

/**
 * Session lifecycle, mirroring mutter's MetaInputCaptureSession:
 *
 *   INIT --Enable()--> ENABLED --barrier crossed--> ACTIVATED
 *     ^                  |                            |
 *     +----Disable()-----+---------Release()----------+
 *
 * Barriers may only be added while INIT. Setting barriers therefore implicitly
 * suspends an enabled session, which is what the spec requires.
 */
enum class session_state_t
{
    INIT,
    ENABLED,
    ACTIVATED,
};

/**
 * One input capture session.
 *
 * A session is created over the private compositor D-Bus interface and owns:
 *  - a D-Bus object at a compositor-chosen path
 *  - its own libei *server* context, so that every EIS client reaching it
 *    belongs to this session by construction (this is how mutter associates
 *    clients with sessions -- eis_backend_fd_add_client() hands back only the
 *    fd, not the eis_client *)
 *  - an EIS seat and the devices bound to it
 *  - the zone set, the barriers, and the enabled/active state machine
 *
 * All state the portal needs to reason about lives here, on the compositor
 * side. The portal is a translation shim only.
 */
class eis_session_t : public std::enable_shared_from_this<eis_session_t>
{
  public:
    eis_session_t(class eis_server_t *server, std::string path,
        uint32_t capabilities, std::string peer_name, uint32_t id);
    ~eis_session_t();

    const std::string& path() const
    {
        return this->object_path;
    }

    uint32_t capabilities() const
    {
        return this->caps;
    }

    class eis_server_t *server() const
    {
        return this->srv;
    }

    /**
     * Check that the sender of @msg is allowed to operate on this session.
     *
     * Modeled on mutter's check_permission(): the unique bus name recorded when
     * the session was created must match the sender of every session-scoped
     * method call.
     */
    bool check_permission(sd_bus_message *msg);

    /** Tear down compositor-side resources (EIS context, seat, devices). */
    void close();

    session_state_t state() const
    {
        return this->session_state;
    }

    // --- zones and barriers ------------------------------------------------

    const std::vector<zone_t>& zones() const
    {
        return this->zone_list;
    }

    uint32_t serial() const
    {
        return this->zone_serial;
    }

    /** Recompute the zone set; bumps the serial and signals if it changed. */
    void refresh_zones();

    /** Add a barrier, returning its compositor-assigned id. 0 means rejected. */
    uint32_t add_barrier(uint32_t serial, int32_t x1, int32_t y1, int32_t x2,
        int32_t y2);

    /** Drop all barriers and return to INIT, suspending an active session. */
    void clear_barriers();

    // --- state machine -----------------------------------------------------

    void enable();
    void disable();
    void release(uint32_t activation_id);

    // --- input path --------------------------------------------------------

    /**
     * Handle a pointer motion event.
     *
     * @a dx/@a dy are the raw, unconstrained motion; @a cx/@a cy the resulting
     * cursor position. Returns true if the event must be withheld from the rest
     * of the compositor (the caller then sets input_event_processing_mode_t::IGNORE).
     */
    bool handle_motion(double dx, double dy, double cx, double cy);

    void note_button(uint32_t button, bool pressed);
    void note_key(uint32_t keycode, bool pressed);
    void send_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked,
        uint32_t group);

    // Emitters, called from the plugin's input taps.
    void emit_motion(double dx, double dy);
    void emit_motion_absolute(double x, double y);
    void emit_button(uint32_t button_code, bool pressed);
    void emit_scroll(double horizontal, double vertical);
    void emit_key(uint32_t keycode, bool pressed);

    /** Emit a signal on this session's D-Bus object. */
    void emit_signal(const char *member, const char *signature, ...);

    /** The libei server context; the D-Bus handler mints client fds from it. */
    eis *context = nullptr;

  private:
    void on_eis_readable();
    void handle_eis_event(struct eis_event *event);

    void setup_client(eis_client *client);
    void ensure_eis_devices();
    eis_device *create_pointer_device();
    eis_device *create_absolute_device();
    eis_device *create_keyboard_device();
    void add_output_regions(eis_device *device);
    void destroy_device(eis_device *device);

    /** Crossing test against every barrier; returns the barrier id or 0. */
    uint32_t find_crossed_barrier(double px, double py, double cx, double cy,
        double *ix, double *iy) const;

    void activate(uint32_t barrier_id, double cx, double cy);
    void deactivate(bool emit_signal);

    /** Release everything logically held, before stopping emulation. */
    void release_held_input();

    void emit_activated(uint32_t barrier_id, double cx, double cy);
    void emit_deactivated();
    void emit_zones_changed();
    void emit_disabled();


    eis_server_t *srv;
    std::string object_path;
    std::string peer_name;
    uint32_t caps;
    uint32_t id;

    sd_bus *bus = nullptr;
    sd_bus_slot *session_slot = nullptr;

    // --- EI state ----------------------------------------------------------

    wl_event_source *eis_source = nullptr;

    eis_client *client = nullptr;
    eis_seat *seat = nullptr;

    /** Relative pointer with button + scroll, carrying the output regions. */
    eis_device *pointer = nullptr;
    /** Absolute pointer with button + scroll, carrying the output regions. */
    eis_device *absolute = nullptr;
    /** Keyboard, carrying the compositor's xkb keymap. */
    eis_device *keyboard = nullptr;

    /** Capabilities the client bound on our seat. */
    uint32_t bound_caps = 0;

    /** Matches the portal-level activation_id, per spec. */
    uint32_t sequence = 0;
    uint32_t activation_id = 0;

    session_state_t session_state = session_state_t::INIT;

    // --- zones / barriers --------------------------------------------------

    std::vector<zone_t> zone_list;
    uint32_t zone_serial = 0;
    std::map<uint32_t, barrier_t> barriers;
    uint32_t next_barrier_id = 1;

    /** Last cursor position seen by the motion tap, for crossing tests. */
    double last_x = 0.0;
    double last_y = 0.0;
    bool have_last = false;

    /** Guards against re-entering the tap from our own cursor warp. */
    bool warping = false;

    // --- logical device state, so we can release it on deactivation ---------

    std::set<uint32_t> keys_pressed;
    std::set<uint32_t> buttons_pressed;
    bool have_modifiers = false;
    uint32_t mods_depressed = 0;
    uint32_t mods_latched = 0;
    uint32_t mods_locked = 0;
    uint32_t mods_group = 0;

  public:
    friend class eis_server_t;
};

/**
 * The compositor-side EIS server plus the private D-Bus interface used to reach
 * it.
 *
 * The server itself owns no EIS context: each session creates its own, so that
 * EIS clients can be attributed to sessions unambiguously.
 */
class eis_server_t
{
  public:
    void init();
    void fini();

    // --- entry points called from the private D-Bus vtables -----------------

    /** Create a session for @peer with @capabilities. */
    eis_session_ptr create_session(uint32_t capabilities, const std::string &peer);

    /** Called when a session's D-Bus object goes away. */
    void session_closed(const eis_session_ptr &session);

    /**
     * The session that is currently activated, i.e. actively capturing.
     * This is the right target for input that must only be diverted while a
     * capture is in progress.
     */
    eis_session_ptr active_session();

    /**
     * The session that is enabled or activated.
     *
     * Pointer motion must be offered to this session even while it is merely
     * ENABLED, because detecting a barrier crossing is what promotes it to
     * ACTIVATED. Taps that only divert use active_session() instead.
     */
    eis_session_ptr enabled_session();

    /** Recompute zones for every session; called when the output layout changes. */
    void zones_changed();

  private:
    void on_bus_readable(int fd, uint32_t mask);
    void on_bus_writable(int fd, uint32_t mask);
    void update_bus_sources();

    sd_bus *bus = nullptr;
    sd_bus_slot *manager_slot = nullptr;
    wl_event_source *bus_read_source = nullptr;
    wl_event_source *bus_write_source = nullptr;

    std::map<std::string, eis_session_ptr> sessions;

    uint32_t next_session_id = 1;
};