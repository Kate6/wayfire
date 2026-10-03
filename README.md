# [Wayfire]

[Wayfire]: https://wayfire.org

![Version](https://img.shields.io/github/v/release/WayfireWM/wayfire)
[![Matrix: #wayfire:matrix.org](https://img.shields.io/badge/matrix-%23wayfire%3Amatrix.org-blue)](https://matrix.to/#/#wayfire:matrix.org)
[![IRC: #wayfire on Libera.chat](https://img.shields.io/badge/IRC-%23wayfire%20at%20libera.chat-green)](https://web.libera.chat/#wayfire)
[![Discord](https://img.shields.io/discord/1144831589877043220?label=Discord)](https://discord.gg/5SWAxmBCUH)
[![CI](https://github.com/WayfireWM/wayfire/workflows/CI/badge.svg)](https://github.com/WayfireWM/wayfire/actions)
[![Packaging status](https://repology.org/badge/tiny-repos/wayfire.svg)](https://repology.org/project/wayfire/versions)
[![License](https://img.shields.io/github/license/WayfireWM/wayfire)](LICENSE)

###### [Get started] | [Manual] | [Configuration]

[Get started]: https://github.com/WayfireWM/wayfire/wiki/Tutorial
[Manual]: https://github.com/WayfireWM/wayfire/wiki/General
[Configuration]: https://github.com/WayfireWM/wayfire/wiki/Configuration

Wayfire is a 3D [Wayland] compositor, inspired by [Compiz] and based on [wlroots].

It aims to create a customizable, extendable and lightweight environment without sacrificing its appearance.

[![Wayfire demos](https://img.youtube.com/vi_webp/2PtNzxDsxYM/maxresdefault.webp)](https://youtube.com/playlist?list=PLb7YRKEhWEBUIoT-a29UoJW9mhfzjpNle "YouTube – Wayfire demos")
[![YouTube Play Button](https://www.iconfinder.com/icons/317714/download/png/16)](https://youtube.com/playlist?list=PLb7YRKEhWEBUIoT-a29UoJW9mhfzjpNle) · [Wayfire demos](https://youtube.com/playlist?list=PLb7YRKEhWEBUIoT-a29UoJW9mhfzjpNle)

[Wayland]: https://wayland.freedesktop.org
[wlroots]: https://github.com/swaywm/wlroots
[Compiz]: https://launchpad.net/compiz

## `eis` plugin (input capture)

> **Note:** this section describes in-progress work on a local branch
> (`input-capture`), not a released Wayfire feature.

`plugins/eis/` implements the compositor half of
`org.freedesktop.impl.portal.InputCapture`, which lets a portal hand the
compositor's keyboard and pointer to a remote machine over the network — the
mechanism Deskflow uses for its KVM portal.

Wayfire acts as the [EIS] server. It owns the EI devices and region geometry,
gives each capture session an `eis` connection over a file descriptor, and
translates pointer barriers between portal coordinates and output-space
coordinates. The public portal API is *not* implemented here; the plugin
exposes a private bus interface, `org.wayfire.Eis`, which the InputCapture
backend in xdg-desktop-portal-wlr sits on top of.

Notable behaviour:

- one `struct eis *` per capture session
- the session's zone is the union of the enabled output work areas, and
  `ZonesChanged` is emitted whenever it moves
- barrier IDs are compositor-owned; the backend maps them in both directions
- the cursor is hidden while capture is active, since the pointer is driving
  the remote machine, and held keys/buttons are released on deactivation so
  modifiers do not stick

Exercised end to end against a real Deskflow client: session creation and
`Start`, the EI handshake yielding pointer and keyboard devices, barriers for
every outer edge, barrier crossing handing capture to the remote machine, and
clean teardown on `Stop`.

Needs `libei` (pkg-config `libeis-1.0`, which provides the server half) and
`libsystemd`.

[EIS]: https://gitlab.freedesktop.org/libei/libei

## Dependencies

### Wayfire Dependencies

These are the dependencies needed for building Wayfire.

- [Cairo](https://cairographics.org)
- [Pango](https://pango.gnome.org/) and PangoCairo
- [FreeType](https://freetype.org)
- [GLM](https://glm.g-truc.net)
- [libdrm](https://dri.freedesktop.org/wiki/DRM/)
- [libevdev](https://freedesktop.org/wiki/Software/libevdev/)
- [libGL](https://mesa3d.org)
- [libinput](https://freedesktop.org/wiki/Software/libinput/)
- [libjpeg](https://libjpeg-turbo.org)
- [libpng](http://libpng.org/pub/png/libpng.html)
- [libxkbcommon](https://xkbcommon.org)
- [libxml2](http://xmlsoft.org/)
- [Pixman](https://pixman.org)
- [pkg-config](https://freedesktop.org/wiki/Software/pkg-config/)
- [Wayland](https://wayland.freedesktop.org)
- [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols)
- [wf-config](https://github.com/WayfireWM/wf-config)
- [wlroots](https://github.com/swaywm/wlroots)

### wlroots Dependencies

These are the dependencies needed for building wlroots, and should be installed before building it.
They are relevant for cases when the system doesn't have a version of wlroots installed.

#### DRM Backend (required)

- [libdisplay-info-dev](https://gitlab.freedesktop.org/emersion/libdisplay-info)
- [hwdata-dev](https://github.com/vcrhonek/hwdata)

#### GLES2 renderer (required)
- [libglvnd](https://gitlab.freedesktop.org/glvnd/libglvnd)
- [mesa](https://gitlab.freedesktop.org/mesa/mesa) (with libEGL and gbm support)

#### Libinput Backend (required)
- [libinput](https://gitlab.freedesktop.org/libinput/libinput)

#### Session Provider (required)

- libudev (via [systemd](https://systemd.io/) **or** other providers)
- [seatd](https://git.sr.ht/~kennylevinsen/seatd)

#### XWayland Support (optional)

- [xcb](https://xcb.freedesktop.org/)
- [xcb-composite](https://xorg.freedesktop.org/wiki/)
- [xcb-render](https://xorg.freedesktop.org/wiki/)
- [xcb-xfixes](https://xorg.freedesktop.org/wiki/)

#### X11 Backend (optional)

- [xcb](https://xcb.freedesktop.org/)
- [x11-xcb](https://xcb.freedesktop.org/)
- [xcb-xinput](https://xorg.freedesktop.org/wiki/)
- [xcb-xfixes](https://xorg.freedesktop.org/wiki/)

## Installation

The easiest way to install Wayfire, wf-shell and WCM to get a functional desktop is to use the [install scripts](https://github.com/WayfireWM/wf-install).

Alternatively, you can build from source:

``` sh
meson build
ninja -C build
sudo ninja -C build install
```

**Note**: `wf-config` and `wlroots` can be built as submodules, by specifying
`-Duse_system_wfconfig=disabled` and `-Duse_system_wlroots=disabled` options to `meson`.
This is the default if they are not present on your system.

Installing [wf-shell](https://github.com/WayfireWM/wf-shell) is recommended for a complete experience.

External plugins can be installed either manually or with `wayfire-plugin`, for example:

```sh
wayfire-plugin install https://github.com/soreau/pixdecor
```

See [Managing External Plugins](docs/wayfire-plugin.md) for more information.

###### Arch Linux

[wayfire](https://aur.archlinux.org/packages/wayfire/) and [wayfire-git] are available in the [AUR].

``` sh
yay -S wayfire
```

[AUR]: https://aur.archlinux.org
[wayfire-git]: https://aur.archlinux.org/packages/wayfire-git/

###### Exherbo

``` sh
cave resolve -x wayfire
```

###### Fedora

``` sh
dnf install wayfire
```

###### FreeBSD
Install the latest release and recommended addons with
``` sh
pkg install wayfire wayfire-plugins-extra wf-shell wcm
```

###### Gentoo
Install the latest release with
```sh
emerge --ask --verbose wayfire
```
and to use the live version
```sh
emerge --ask --verbose "=gui-wm/wayfire-9999"
```

###### NixOS

Enable Wayfire in your NixOS configuration:
```nix
programs.wayfire = {
  enable = true;
  plugins = with pkgs.wayfirePlugins; [
    wcm
    wf-shell
    wayfire-plugins-extra
  ];
};
```

###### Ubuntu/Debian 13

```
apt install wayfire
```

###### Void

``` sh
xbps-install -S wayfire
```

## Configuration

Copy [`wayfire.ini`] to `~/.config/wayfire.ini` or `~/.config/wayfire/wayfire.ini`.
Before running Wayfire, you may want to change the command to start a terminal.
See the [Configuration] document for information on the options.

[`wayfire.ini`]: wayfire.ini

## Running

Run [`wayfire`][Manual] from a TTY, or via a Wayland-compatible login manager.
