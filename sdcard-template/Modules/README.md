Drop `.so` dynamic modules here, list them in MODULES_ENABLED, and GhostLink
will dlopen each on boot. Each module exports `int ghost_module_init(void)`.
Also used for WireGuard config, MITM CA, inject.js, etc. — see config.txt.
