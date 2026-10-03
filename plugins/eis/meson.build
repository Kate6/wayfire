# This is the *server* half of libei. The pkg-config name is "libeis-1.0" but the
# library file is libeis.so (-leis); libei-1.0 is the client half and exports none of
# the eis_* symbols we need.
libeis = dependency('libeis-1.0')
sdbus = dependency('libsystemd')

eis_deps = [wlroots, pixman, wfconfig, xkbcommon, plugin_pch_dep, libeis, sdbus]

eis = shared_module('eis',
    ['eis.cpp'],
    include_directories: [wayfire_api_inc, wayfire_conf_inc, plugins_common_inc],
    dependencies: eis_deps,
    install: true,
    install_dir: conf_data.get('PLUGIN_PATH'))