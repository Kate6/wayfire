focus_follows_mouse = shared_module('focus-follows-mouse',
    ['focus-follows-mouse.cpp'],
    include_directories: [wayfire_api_inc, wayfire_conf_inc, plugins_common_inc],
    dependencies: [wlroots, pixman, wfconfig, plugin_pch_dep],
    install: true,
    install_dir: conf_data.get('PLUGIN_PATH'))