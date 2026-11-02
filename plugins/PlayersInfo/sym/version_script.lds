{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* levels_ranks binding slots (defined by levels_ranks_bindings, filled by the plugify
           module from the levels_ranks plugin's exported methods) */
        __levels_ranks_*;

        /* No exported methods of our own: PlayersInfo exposes nothing to other plugins.
           No __hudmenu_* either - the plugin does not use pisexcompat menus (PLUGIN_STANDARD §7). */

    local: *;
};
