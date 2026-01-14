# Plan: Instance-Based Plugin API v2

## TL;DR

- **v1 plugins (old)**: Single instance only. Fourtrack REFUSES to load same v1 plugin on multiple tracks.
- **v2 plugins (new)**: Multiple instances allowed. Each track gets independent state.

## Problem

Current plugin API v1 uses global state - all functions operate on implicit global variables. When the same plugin is loaded multiple times (e.g., JV880 on tracks 1 and 2), they share the same memory, causing corruption and distorted audio.

## Solution: Instance-Based API

Add a v2 API where plugins return an instance pointer from `create_instance`, and all subsequent calls pass that instance pointer. Plugins allocate per-instance state instead of using globals.

## API Changes

### Current v1 (global state)
```c
typedef struct plugin_api_v1 {
    int (*on_load)(const char *module_dir, const char *json_defaults);
    void (*on_unload)(void);
    void (*on_midi)(const uint8_t *msg, int len, int source);
    void (*set_param)(const char *key, const char *val);
    int (*get_param)(const char *key, char *buf, int buf_len);
    void (*render_block)(int16_t *out_interleaved_lr, int frames);
} plugin_api_v1_t;
```

### New v2 (instance-based)
```c
#define MOVE_PLUGIN_API_VERSION 2

typedef struct plugin_api_v2 {
    uint32_t api_version;

    /* Create instance - returns opaque instance pointer, or NULL on failure */
    void* (*create_instance)(const char *module_dir, const char *json_defaults);

    /* Destroy instance */
    void (*destroy_instance)(void *instance);

    /* All callbacks take instance as first parameter */
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

/* New init symbol */
typedef plugin_api_v2_t* (*move_plugin_init_v2_fn)(const host_api_v1_t *host);
#define MOVE_PLUGIN_INIT_V2_SYMBOL "move_plugin_init_v2"
```

## Affected Repositories

### 1. move-anything (host framework)
- Add `plugin_api_v2.h` with new API
- Update `chain_host.c` to detect and use v2 plugins
- Keep v1 support for backwards compatibility
- Files: `src/host/plugin_api_v2.h`, `src/modules/chain/dsp/chain_host.c`

### 2. move-anything-fourtrack (this repo)
- Update to use v2 API when loading synths
- Store instance pointer per track
- Pass instance to all plugin calls
- Files: `src/dsp/fourtrack.c`, `src/dsp/plugin_api_v1.h` (copy v2)

### 3. Sound Generator Modules (each repo)
- **move-anything-sf2**: Refactor TinySoundFont state to instance struct
- **move-anything-dx7**: Refactor Dexed/MSFA state to instance struct
- **move-anything-jv880**: Refactor JV880 emulator state to instance struct
- **move-anything-obxd**: Refactor OB-Xd state to instance struct
- **move-anything-clap**: Refactor CLAP host state to instance struct

### 4. Audio FX Modules (each repo)
- **move-anything-cloudseed**: Refactor reverb state to instance struct
- **move-anything-psxverb**: Refactor PSX reverb state to instance struct
- **move-anything-tapescam**: Refactor tape state to instance struct
- **move-anything-space-delay**: Refactor delay state to instance struct

### 5. Internal Sound Generators (in move-anything)
- **linein**: Already stateless, trivial update
- **freeverb**: Refactor to instance struct

## Implementation Order

### Phase 1: API Definition
1. Create `plugin_api_v2.h` in move-anything
2. Document migration guide

### Phase 2: Host Support
3. Update chain_host.c to support v2 (with v1 fallback)
4. Update fourtrack.c to use v2 API

### Phase 3: Plugin Migration (can be parallel)
5. Migrate linein (simplest, good template)
6. Migrate sf2
7. Migrate dx7
8. Migrate jv880
9. Migrate obxd
10. Migrate audio FX modules
11. Migrate clap

### Phase 4: Cleanup
12. Test all combinations
13. Update documentation

## Plugin Migration Pattern

For each plugin, the migration follows this pattern:

### Before (v1 - globals)
```c
static some_synth_t g_synth;
static int g_preset;

int on_load(const char *dir, const char *json) {
    init_synth(&g_synth);
    return 0;
}

void render_block(int16_t *out, int frames) {
    synth_render(&g_synth, out, frames);
}

plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host) {
    return &g_api;
}
```

### After (v2 - instance)
```c
typedef struct {
    some_synth_t synth;
    int preset;
} my_instance_t;

void* create_instance(const char *dir, const char *json) {
    my_instance_t *inst = calloc(1, sizeof(my_instance_t));
    init_synth(&inst->synth);
    return inst;
}

void destroy_instance(void *instance) {
    my_instance_t *inst = (my_instance_t*)instance;
    cleanup_synth(&inst->synth);
    free(inst);
}

void render_block(void *instance, int16_t *out, int frames) {
    my_instance_t *inst = (my_instance_t*)instance;
    synth_render(&inst->synth, out, frames);
}

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    return &g_api_v2;
}
```

## Backwards Compatibility Strategy

**Key Rule: v1 plugins = single instance only, v2 plugins = multiple instances allowed**

### How Fourtrack Handles This

1. When loading a synth for a track, check if plugin exports `move_plugin_init_v2`
2. If **v2 available**: Use v2 API, allow multiple instances across tracks
3. If **v1 only**:
   - Check if this module is already loaded on another track
   - If yes: **REFUSE** with error "Module X already in use on track Y (upgrade to v2 for multi-instance)"
   - If no: Load normally as single instance

### Plugin Migration Path

- Plugins can export BOTH symbols during transition
- Old hosts use v1, new hosts prefer v2
- Once all hosts support v2, plugins can drop v1

### Detection Code (in fourtrack)
```c
/* Try v2 first */
move_plugin_init_v2_fn init_v2 = dlsym(handle, "move_plugin_init_v2");
if (init_v2) {
    /* v2 plugin - supports multiple instances */
    track->plugin_v2 = init_v2(&host_api);
    track->instance = track->plugin_v2->create_instance(module_dir, NULL);
} else {
    /* v1 plugin - single instance only */
    move_plugin_init_v1_fn init_v1 = dlsym(handle, "move_plugin_init_v1");

    /* Check if already loaded on another track */
    for (int i = 0; i < NUM_TRACKS; i++) {
        if (i != track_idx && strcmp(g_tracks[i].synth_module, module_name) == 0) {
            ft_log("ERROR: v1 plugin already in use - upgrade plugin for multi-instance");
            return -1;
        }
    }

    track->plugin_v1 = init_v1(&host_api);
    track->plugin_v1->on_load(module_dir, NULL);
}
```

## Estimated Effort

- API definition: 1 hour
- Fourtrack v2 support + v1 refusal: 2-3 hours
- Each plugin migration: 1-3 hours depending on complexity
- Total: ~20-30 hours across all repos

## Immediate Next Step

Before migrating any plugins, update fourtrack to:
1. Detect v1 vs v2 plugins
2. **Refuse** duplicate v1 plugins with clear error message
3. Support v2 plugins with multiple instances

This gives users a working (if limited) experience while plugins are migrated.
