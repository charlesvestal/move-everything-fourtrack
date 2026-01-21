/*
 * Four Track Recorder DSP
 *
 * A 4-track audio recorder that can load signal chain patches as track sources.
 * One track can be active at a time for live playing/recording, while others play back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#include "plugin_api_v1.h"
/* Note: Audio FX are now handled by chain instances, not directly by fourtrack */

/* ============================================================================
 * Constants
 * ============================================================================ */

#define NUM_TRACKS 4
#define SAMPLE_RATE 44100
#define FRAMES_PER_BLOCK 128
#define NUM_CHANNELS 2

/* Recording buffer: 5 minutes per track at 44.1kHz stereo
 * Memory usage: ~176KB per second per track (stereo int16)
 * 300s × 4 tracks = ~210MB
 */
#define MAX_RECORD_SECONDS 300  /* 5 minutes max per track */
#define MAX_RECORD_SAMPLES (MAX_RECORD_SECONDS * SAMPLE_RATE)
#define TRACK_BUFFER_SIZE (MAX_RECORD_SAMPLES * NUM_CHANNELS)

static int g_record_seconds = MAX_RECORD_SECONDS;

/* Path limits */
#define MAX_PATH_LEN 512
#define MAX_NAME_LEN 64
#define MAX_PATCHES 64
#define MAX_AUDIO_FX 4   /* Max audio FX per track */

/* ============================================================================
 * Types
 * ============================================================================ */

/* Knob mapping constants */
#define MAX_KNOB_MAPPINGS 8
#define KNOB_CC_START 71
#define KNOB_CC_END 78
#define KNOB_STEP_FLOAT 0.05f  /* Step size for float params */
#define KNOB_STEP_INT 1        /* Step size for int params */

/* Knob mapping types */
typedef enum {
    KNOB_TYPE_FLOAT = 0,
    KNOB_TYPE_INT = 1
} knob_type_t;

/* Knob mapping structure */
typedef struct {
    int cc;              /* CC number (71-78) */
    char target[16];     /* "synth", "fx1", etc - we only support "synth" for now */
    char param[32];      /* Parameter key */
    char name[32];       /* Display name for overlay */
    knob_type_t type;    /* Parameter type (float or int) */
    float min_val;       /* Minimum value */
    float max_val;       /* Maximum value */
    float current_value; /* Current parameter value */
} knob_mapping_t;

/* Track state */
typedef struct {
    int16_t *buffer;           /* Audio buffer (stereo interleaved) */
    int length;                /* Recorded length in samples (not frames) */
    float level;               /* Track level 0.0-1.0 */
    float pan;                 /* Pan -1.0 (L) to +1.0 (R) */
    int muted;                 /* Mute state */
    int solo;                  /* Solo state */
    int armed;                 /* Armed for recording */
    int monitoring;            /* Monitoring live input */
    char patch_name[MAX_NAME_LEN];  /* Associated chain patch name */
    char patch_path[MAX_PATH_LEN];  /* Full path to patch file */
    /* Per-track chain instance (includes synth + audio FX + MIDI FX) */
    void *chain_handle;              /* dlopen handle for chain module */
    plugin_api_v2_t *chain_plugin;   /* chain v2 API */
    void *chain_instance;            /* chain instance pointer */
    int chain_patch_idx;             /* Current patch index within chain */
    /* Per-track knob mappings - handled by chain, but we cache for UI */
    knob_mapping_t knob_mappings[MAX_KNOB_MAPPINGS];
    int knob_mapping_count;
} track_t;

/* Patch info for browser */
typedef struct {
    char name[MAX_NAME_LEN];
    char path[MAX_PATH_LEN];
} patch_info_t;

/* Transport state */
typedef enum {
    TRANSPORT_STOPPED,
    TRANSPORT_PLAYING,
    TRANSPORT_RECORDING,
    TRANSPORT_COUNTIN      /* Count-in before recording */
} transport_state_t;

/* ============================================================================
 * Globals
 * ============================================================================ */

static const host_api_v1_t *g_host = NULL;
static char g_module_dir[MAX_PATH_LEN];

/* Tracks */
static track_t g_tracks[NUM_TRACKS];
static int g_selected_track = 0;          /* Currently selected track (0-3) */

/* Transport */
static transport_state_t g_transport = TRANSPORT_STOPPED;
static int g_playhead = 0;                /* Current playback position in samples */
static int g_loop_start = 0;              /* Loop start position */
static int g_loop_end = 0;                /* Loop end position (0 = no loop) */
static int g_loop_enabled = 0;            /* Loop mode enabled */

/* Chain patch browser */
static patch_info_t g_patches[MAX_PATCHES];
static int g_patch_count = 0;

/* Metronome - beat position derived directly from playhead */
static int g_metronome_enabled = 0;
static int g_tempo_bpm = 120;
static int g_samples_per_beat = 0;

/* Count-in - uses separate counter since playhead doesn't move during count-in */
static int g_countin_enabled = 0;
static int g_countin_counter = 0;          /* Counts up during count-in (samples) */
static int g_countin_total_samples = 0;    /* Total samples for count-in (4 beats) */

/* Project file */
static char g_project_path[MAX_PATH_LEN];
static int g_project_loaded = 0;

/* Solo tracking */
static int g_any_solo = 0;

/* MIDI routing mode */
typedef enum {
    MIDI_ROUTING_SELECTED = 0,   /* All MIDI goes to selected track (default) */
    MIDI_ROUTING_SPLIT_CHANNELS  /* External MIDI split by channel: ch1→track1, etc. */
} midi_routing_mode_t;

static midi_routing_mode_t g_midi_routing_mode = MIDI_ROUTING_SELECTED;

/* Last error message (for UI display) */
static char g_last_error[256] = "";

/* ============================================================================
 * Logging
 * ============================================================================ */

static void ft_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[512];
        snprintf(buf, sizeof(buf), "fourtrack: %s", msg);
        g_host->log(buf);
    }
}

/* ============================================================================
 * Chain Integration
 * ============================================================================ */

/* Host API forwarding for loaded synth plugins */
static host_api_v1_t g_subplugin_host_api;

static void subplugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[512];
        snprintf(buf, sizeof(buf), "fourtrack-chain: %s", msg);
        g_host->log(buf);
    }
}

static int subplugin_midi_send_internal(const uint8_t *msg, int len) {
    if (g_host && g_host->midi_send_internal) {
        return g_host->midi_send_internal(msg, len);
    }
    return 0;
}

static int subplugin_midi_send_external(const uint8_t *msg, int len) {
    if (g_host && g_host->midi_send_external) {
        return g_host->midi_send_external(msg, len);
    }
    return 0;
}

/* Get track index from track pointer */
static int get_track_index(track_t *track) {
    for (int i = 0; i < NUM_TRACKS; i++) {
        if (&g_tracks[i] == track) return i;
    }
    return -1;
}

/* Load chain module for a track - chain handles synth + audio FX + MIDI FX */
static int load_chain_for_track(track_t *track) {
    char msg[256];
    char chain_path[MAX_PATH_LEN];
    int track_idx = get_track_index(track);

    /* Use absolute path to chain module */
    snprintf(chain_path, sizeof(chain_path),
             "/data/UserData/move-anything/modules/chain/dsp.so");

    snprintf(msg, sizeof(msg), "Loading chain from: %s", chain_path);
    ft_log(msg);

    /* Open the chain module */
    track->chain_handle = dlopen(chain_path, RTLD_NOW | RTLD_LOCAL);
    if (!track->chain_handle) {
        snprintf(msg, sizeof(msg), "dlopen chain failed: %s", dlerror());
        ft_log(msg);
        return -1;
    }

    /* Chain must support v2 API for multi-instance */
    move_plugin_init_v2_fn init_v2 = (move_plugin_init_v2_fn)dlsym(track->chain_handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (!init_v2) {
        ft_log("Chain module does not support v2 API - cannot use multi-instance");
        dlclose(track->chain_handle);
        track->chain_handle = NULL;
        return -1;
    }

    track->chain_plugin = init_v2(&g_subplugin_host_api);
    if (!track->chain_plugin) {
        ft_log("Chain plugin v2 init returned NULL");
        dlclose(track->chain_handle);
        track->chain_handle = NULL;
        return -1;
    }

    /* Use absolute path to chain module directory */
    char chain_dir[MAX_PATH_LEN];
    snprintf(chain_dir, sizeof(chain_dir),
             "/data/UserData/move-anything/modules/chain");

    /* Create chain instance */
    track->chain_instance = track->chain_plugin->create_instance(chain_dir, NULL);
    if (!track->chain_instance) {
        ft_log("Chain create_instance returned NULL");
        dlclose(track->chain_handle);
        track->chain_handle = NULL;
        track->chain_plugin = NULL;
        return -1;
    }

    track->chain_patch_idx = -1;
    snprintf(msg, sizeof(msg), "Chain instance created for track %d", track_idx + 1);
    ft_log(msg);

    return 0;
}

/* Unload chain for a track */
static void unload_chain_for_track(track_t *track) {
    if (track->chain_plugin && track->chain_instance) {
        if (track->chain_plugin->destroy_instance) {
            track->chain_plugin->destroy_instance(track->chain_instance);
        }
        track->chain_instance = NULL;
    }

    if (track->chain_handle) {
        dlclose(track->chain_handle);
    }
    track->chain_handle = NULL;
    track->chain_plugin = NULL;
    track->chain_patch_idx = -1;
}

/* Load a patch into a track's chain instance */
static int load_patch_for_track(track_t *track, int patch_idx) {
    char msg[256];
    int track_idx = get_track_index(track);

    if (!track->chain_plugin || !track->chain_instance) {
        ft_log("Cannot load patch - no chain instance");
        return -1;
    }

    /* Tell chain to load the patch */
    char patch_str[16];
    snprintf(patch_str, sizeof(patch_str), "%d", patch_idx);
    track->chain_plugin->set_param(track->chain_instance, "load_patch", patch_str);

    track->chain_patch_idx = patch_idx;

    snprintf(msg, sizeof(msg), "Track %d: loaded patch index %d", track_idx + 1, patch_idx);
    ft_log(msg);

    return 0;
}

/* Forward declarations */
static void chain_panic_for_track(track_t *track);

/* ============================================================================
 * JSON Parsing Helpers
 * ============================================================================ */

/* Simple JSON string extraction - finds "key": "value" and copies value to buf */
static int json_get_string(const char *json, const char *key, char *buf, int buf_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    /* Find the colon */
    const char *colon = strchr(pos + strlen(search), ':');
    if (!colon) return -1;

    /* Find opening quote */
    const char *quote1 = strchr(colon, '"');
    if (!quote1) return -1;
    quote1++;

    /* Find closing quote */
    const char *quote2 = strchr(quote1, '"');
    if (!quote2) return -1;

    int len = quote2 - quote1;
    if (len >= buf_len) len = buf_len - 1;
    strncpy(buf, quote1, len);
    buf[len] = '\0';
    return 0;
}

/* Extract string from nested section: "section": { "key": "value" } */
static int json_get_string_in_section(const char *json, const char *section,
                                      const char *key, char *buf, int buf_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", section);

    const char *section_pos = strstr(json, search);
    if (!section_pos) return -1;

    /* Find opening brace of section */
    const char *brace = strchr(section_pos, '{');
    if (!brace) return -1;

    /* Find closing brace */
    const char *end_brace = strchr(brace, '}');
    if (!end_brace) return -1;

    /* Search for key within section */
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *key_pos = strstr(brace, search);
    if (!key_pos || key_pos > end_brace) return -1;

    /* Find the colon */
    const char *colon = strchr(key_pos + strlen(search), ':');
    if (!colon || colon > end_brace) return -1;

    /* Find opening quote */
    const char *quote1 = strchr(colon, '"');
    if (!quote1 || quote1 > end_brace) return -1;
    quote1++;

    /* Find closing quote */
    const char *quote2 = strchr(quote1, '"');
    if (!quote2 || quote2 > end_brace) return -1;

    int len = quote2 - quote1;
    if (len >= buf_len) len = buf_len - 1;
    strncpy(buf, quote1, len);
    buf[len] = '\0';
    return 0;
}

/* Extract int from nested section */
static int json_get_int_in_section(const char *json, const char *section,
                                   const char *key, int *val) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", section);

    const char *section_pos = strstr(json, search);
    if (!section_pos) return -1;

    const char *brace = strchr(section_pos, '{');
    if (!brace) return -1;

    const char *end_brace = strchr(brace, '}');
    if (!end_brace) return -1;

    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *key_pos = strstr(brace, search);
    if (!key_pos || key_pos > end_brace) return -1;

    const char *colon = strchr(key_pos + strlen(search), ':');
    if (!colon || colon > end_brace) return -1;

    *val = atoi(colon + 1);
    return 0;
}

/* Extract float from JSON object (between braces) */
static int json_get_float_in_obj(const char *start, const char *end,
                                 const char *key, float *val) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *key_pos = strstr(start, search);
    if (!key_pos || key_pos > end) return -1;

    const char *colon = strchr(key_pos + strlen(search), ':');
    if (!colon || colon > end) return -1;

    *val = (float)atof(colon + 1);
    return 0;
}

/* Parse knob_mappings array from JSON for a track */
static void parse_knob_mappings(track_t *track, const char *json) {
    char msg[256];

    /* Clear existing mappings */
    track->knob_mapping_count = 0;
    memset(track->knob_mappings, 0, sizeof(track->knob_mappings));

    /* Find "knob_mappings" array */
    const char *mappings_pos = strstr(json, "\"knob_mappings\"");
    if (!mappings_pos) {
        ft_log("No knob_mappings in patch");
        return;
    }

    /* Find array start */
    const char *arr_start = strchr(mappings_pos, '[');
    if (!arr_start) return;

    /* Find array end */
    const char *arr_end = strchr(arr_start, ']');
    if (!arr_end) return;

    /* Parse each object in array */
    const char *obj_start = arr_start;
    while (track->knob_mapping_count < MAX_KNOB_MAPPINGS) {
        obj_start = strchr(obj_start + 1, '{');
        if (!obj_start || obj_start > arr_end) break;

        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end || obj_end > arr_end) break;

        /* Extract cc number */
        int cc = 0;
        const char *cc_pos = strstr(obj_start, "\"cc\"");
        if (cc_pos && cc_pos < obj_end) {
            const char *colon = strchr(cc_pos, ':');
            if (colon && colon < obj_end) {
                cc = atoi(colon + 1);
            }
        }

        /* Extract target */
        char target[16] = "";
        const char *target_pos = strstr(obj_start, "\"target\"");
        if (target_pos && target_pos < obj_end) {
            const char *colon = strchr(target_pos, ':');
            if (colon && colon < obj_end) {
                const char *q1 = strchr(colon, '"');
                if (q1 && q1 < obj_end) {
                    const char *q2 = strchr(q1 + 1, '"');
                    if (q2 && q2 < obj_end) {
                        int len = q2 - q1 - 1;
                        if (len > 15) len = 15;
                        strncpy(target, q1 + 1, len);
                        target[len] = '\0';
                    }
                }
            }
        }

        /* Extract param */
        char param[32] = "";
        const char *param_pos = strstr(obj_start, "\"param\"");
        if (param_pos && param_pos < obj_end) {
            const char *colon = strchr(param_pos, ':');
            if (colon && colon < obj_end) {
                const char *q1 = strchr(colon, '"');
                if (q1 && q1 < obj_end) {
                    const char *q2 = strchr(q1 + 1, '"');
                    if (q2 && q2 < obj_end) {
                        int len = q2 - q1 - 1;
                        if (len > 31) len = 31;
                        strncpy(param, q1 + 1, len);
                        param[len] = '\0';
                    }
                }
            }
        }

        /* Extract optional name (for display) */
        char name[32] = "";
        const char *name_pos = strstr(obj_start, "\"name\"");
        if (name_pos && name_pos < obj_end) {
            const char *colon = strchr(name_pos, ':');
            if (colon && colon < obj_end) {
                const char *q1 = strchr(colon, '"');
                if (q1 && q1 < obj_end) {
                    const char *q2 = strchr(q1 + 1, '"');
                    if (q2 && q2 < obj_end) {
                        int len = q2 - q1 - 1;
                        if (len > 31) len = 31;
                        strncpy(name, q1 + 1, len);
                        name[len] = '\0';
                    }
                }
            }
        }

        /* Extract optional type (default: float) */
        knob_type_t type = KNOB_TYPE_FLOAT;
        const char *type_pos = strstr(obj_start, "\"type\"");
        if (type_pos && type_pos < obj_end) {
            const char *colon = strchr(type_pos, ':');
            if (colon && colon < obj_end) {
                const char *q1 = strchr(colon, '"');
                if (q1 && q1 < obj_end) {
                    if (strncmp(q1 + 1, "int", 3) == 0) {
                        type = KNOB_TYPE_INT;
                    }
                }
            }
        }

        /* Extract optional min/max (defaults: 0.0-1.0) */
        float min_val = 0.0f;
        float max_val = 1.0f;
        json_get_float_in_obj(obj_start, obj_end, "min", &min_val);
        json_get_float_in_obj(obj_start, obj_end, "max", &max_val);

        /* Extract optional initial value */
        float current_value = min_val;
        if (json_get_float_in_obj(obj_start, obj_end, "value", &current_value) != 0) {
            current_value = (min_val + max_val) / 2.0f;  /* Default to midpoint */
        }

        /* Store mapping if valid */
        if (cc >= KNOB_CC_START && cc <= KNOB_CC_END && param[0]) {
            knob_mapping_t *m = &track->knob_mappings[track->knob_mapping_count];
            m->cc = cc;
            strncpy(m->target, target[0] ? target : "synth", 15);
            strncpy(m->param, param, 31);
            strncpy(m->name, name[0] ? name : param, 31);  /* Use param as name if none */
            m->type = type;
            m->min_val = min_val;
            m->max_val = max_val;
            m->current_value = current_value;
            track->knob_mapping_count++;

            snprintf(msg, sizeof(msg), "Knob %d: %s -> %s (%.2f-%.2f)",
                     cc - KNOB_CC_START + 1, m->name, m->param, min_val, max_val);
            ft_log(msg);
        }

        obj_start = obj_end;
    }

    snprintf(msg, sizeof(msg), "Loaded %d knob mappings", track->knob_mapping_count);
    ft_log(msg);
}

/* Load a chain patch for a track - using chain instance v2 API */
static int load_chain_patch_for_track(track_t *track, const char *patch_path) {
    char msg[256];
    int track_idx = get_track_index(track);
    (void)patch_path;  /* Patch path not used directly - chain handles it by index */

    /* Ensure chain instance exists */
    if (!track->chain_instance || !track->chain_plugin) {
        ft_log("Cannot load patch - no chain instance");
        return -1;
    }

    /* Chain instance has already scanned patches - find the patch index that matches
     * our patch name, then tell chain to load it by index.
     * First, query how many patches the chain has */
    char count_buf[16];
    if (track->chain_plugin->get_param(track->chain_instance, "patch_count", count_buf, sizeof(count_buf)) < 0) {
        ft_log("Failed to get patch count from chain");
        return -1;
    }
    int patch_count = atoi(count_buf);

    /* Find the patch by name */
    int found_idx = -1;
    for (int i = 0; i < patch_count; i++) {
        char key[32], name_buf[MAX_NAME_LEN];
        snprintf(key, sizeof(key), "patch_name_%d", i);
        if (track->chain_plugin->get_param(track->chain_instance, key, name_buf, sizeof(name_buf)) >= 0) {
            if (strcmp(name_buf, track->patch_name) == 0) {
                found_idx = i;
                break;
            }
        }
    }

    if (found_idx < 0) {
        snprintf(msg, sizeof(msg), "Patch '%s' not found in chain", track->patch_name);
        ft_log(msg);
        return -1;
    }

    /* Tell chain to load the patch */
    char idx_str[16];
    snprintf(idx_str, sizeof(idx_str), "%d", found_idx);
    track->chain_plugin->set_param(track->chain_instance, "load_patch", idx_str);
    track->chain_patch_idx = found_idx;

    snprintf(msg, sizeof(msg), "Track %d: loaded chain patch '%s' (index %d)",
             track_idx + 1, track->patch_name, found_idx);
    ft_log(msg);

    /* Knob mappings are now handled by the chain instance */
    track->knob_mapping_count = 0;  /* We don't cache them anymore */

    return 0;
}

/* ============================================================================
 * Patch Scanning
 * ============================================================================ */

static void scan_patches(void) {
    char patches_dir[MAX_PATH_LEN];
    char msg[256];

    /* Use absolute path to patches directory */
    strncpy(patches_dir, "/data/UserData/move-anything/patches", sizeof(patches_dir) - 1);
    patches_dir[sizeof(patches_dir) - 1] = '\0';

    g_patch_count = 0;

    DIR *dir = opendir(patches_dir);
    if (!dir) {
        snprintf(msg, sizeof(msg), "Cannot open patches dir: %s", patches_dir);
        ft_log(msg);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && g_patch_count < MAX_PATCHES) {
        /* Skip hidden files and non-json files */
        if (entry->d_name[0] == '.') continue;

        size_t len = strlen(entry->d_name);
        if (len < 5 || strcmp(entry->d_name + len - 5, ".json") != 0) continue;

        /* Build full path */
        snprintf(g_patches[g_patch_count].path, MAX_PATH_LEN,
                 "%s/%s", patches_dir, entry->d_name);

        /* Try to read the "name" field from JSON, fall back to filename */
        char patch_name[MAX_NAME_LEN] = "";
        FILE *pf = fopen(g_patches[g_patch_count].path, "r");
        if (pf) {
            char json_buf[1024];
            size_t read_len = fread(json_buf, 1, sizeof(json_buf) - 1, pf);
            json_buf[read_len] = '\0';
            fclose(pf);

            /* Extract "name" field */
            if (json_get_string(json_buf, "name", patch_name, MAX_NAME_LEN) != 0) {
                patch_name[0] = '\0';  /* Not found */
            }
        }

        /* Use JSON name if found, otherwise use filename without .json */
        if (patch_name[0]) {
            strncpy(g_patches[g_patch_count].name, patch_name, MAX_NAME_LEN - 1);
        } else {
            strncpy(g_patches[g_patch_count].name, entry->d_name, MAX_NAME_LEN - 1);
            g_patches[g_patch_count].name[len - 5] = '\0';
        }

        g_patch_count++;
    }

    closedir(dir);

    /* Sort patches alphabetically by name */
    if (g_patch_count > 1) {
        /* Simple qsort comparison function inline via a static helper */
        for (int i = 0; i < g_patch_count - 1; i++) {
            for (int j = i + 1; j < g_patch_count; j++) {
                if (strcasecmp(g_patches[i].name, g_patches[j].name) > 0) {
                    patch_info_t tmp = g_patches[i];
                    g_patches[i] = g_patches[j];
                    g_patches[j] = tmp;
                }
            }
        }
    }

    snprintf(msg, sizeof(msg), "Found %d patches", g_patch_count);
    ft_log(msg);
}

/* Find a patch by name and return its index, or -1 if not found */
static int find_patch_by_name(const char *name) {
    for (int i = 0; i < g_patch_count; i++) {
        if (strcmp(g_patches[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Load default patch for all tracks */
static void load_default_patches(void) {
    int linein_idx = find_patch_by_name("Line In");
    if (linein_idx < 0) {
        ft_log("Line In patch not found, tracks will start empty");
        return;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Loading Line In as default for all tracks");
    ft_log(msg);

    for (int i = 0; i < NUM_TRACKS; i++) {
        /* First create chain instance for this track */
        if (load_chain_for_track(&g_tracks[i]) != 0) {
            snprintf(msg, sizeof(msg), "Track %d: failed to create chain instance", i + 1);
            ft_log(msg);
            continue;
        }

        strncpy(g_tracks[i].patch_name, g_patches[linein_idx].name, MAX_NAME_LEN - 1);
        strncpy(g_tracks[i].patch_path, g_patches[linein_idx].path, MAX_PATH_LEN - 1);
        /* Load chain patch for this track */
        if (load_chain_patch_for_track(&g_tracks[i], g_patches[linein_idx].path) == 0) {
            snprintf(msg, sizeof(msg), "Track %d: Line In loaded", i + 1);
            ft_log(msg);
        }
    }
}

/* ============================================================================
 * Track Management
 * ============================================================================ */

static void clear_track(int track) {
    if (track < 0 || track >= NUM_TRACKS) return;

    if (g_tracks[track].buffer) {
        memset(g_tracks[track].buffer, 0, TRACK_BUFFER_SIZE * sizeof(int16_t));
    }
    g_tracks[track].length = 0;
}

static void init_tracks(void) {
    for (int i = 0; i < NUM_TRACKS; i++) {
        g_tracks[i].buffer = (int16_t *)calloc(TRACK_BUFFER_SIZE, sizeof(int16_t));
        g_tracks[i].length = 0;
        g_tracks[i].level = 0.8f;
        g_tracks[i].pan = 0.0f;
        g_tracks[i].muted = 0;
        g_tracks[i].solo = 0;
        g_tracks[i].armed = 0;
        g_tracks[i].monitoring = (i == 0) ? 1 : 0;  /* Only track 1 has monitoring on by default */
        g_tracks[i].patch_name[0] = '\0';
        g_tracks[i].patch_path[0] = '\0';
        g_tracks[i].chain_handle = NULL;
        g_tracks[i].chain_plugin = NULL;
        g_tracks[i].chain_instance = NULL;
        g_tracks[i].chain_patch_idx = -1;
        g_tracks[i].knob_mapping_count = 0;
    }
}

static void free_tracks(void) {
    for (int i = 0; i < NUM_TRACKS; i++) {
        /* Unload chain for this track */
        unload_chain_for_track(&g_tracks[i]);
        if (g_tracks[i].buffer) {
            free(g_tracks[i].buffer);
            g_tracks[i].buffer = NULL;
        }
    }
}

static void update_solo_state(void) {
    g_any_solo = 0;
    for (int i = 0; i < NUM_TRACKS; i++) {
        if (g_tracks[i].solo) {
            g_any_solo = 1;
            break;
        }
    }
}

/* ============================================================================
 * Transport
 * ============================================================================ */

static void stop_transport(void) {
    g_transport = TRANSPORT_STOPPED;
    /* Keep playhead where it is for punch-in recording */
}

static void start_playback(void) {
    g_transport = TRANSPORT_PLAYING;
}

static int any_track_armed(void) {
    for (int i = 0; i < NUM_TRACKS; i++) {
        if (g_tracks[i].armed) return 1;
    }
    return 0;
}

static void start_recording(void) {
    if (!any_track_armed()) {
        ft_log("No track armed for recording");
        return;
    }

    /* Count-in only when starting from stopped - if already playing, punch in immediately */
    if (g_countin_enabled && g_transport == TRANSPORT_STOPPED) {
        /* Start count-in phase - 4 beats before recording
         * Count-in uses its own counter (playhead stays put)
         * We snap to next beat boundary so count-in clicks are on the grid */
        int beat_pos = g_playhead % g_samples_per_beat;
        int samples_to_next_beat = (beat_pos == 0) ? 0 : (g_samples_per_beat - beat_pos);

        g_countin_counter = -samples_to_next_beat;  /* Start negative to reach beat boundary */
        g_countin_total_samples = 4 * g_samples_per_beat;  /* 4 full beats of count-in */
        g_transport = TRANSPORT_COUNTIN;
        ft_log("Count-in started (4 beats)");
    } else {
        /* Punch-in: just start recording at current playhead position.
         * The recording code overwrites buffer at playhead and extends
         * track.length only if we record past the current length. */
        g_transport = TRANSPORT_RECORDING;
        ft_log("Recording started (punch-in)");
    }
}

/* Transition from count-in to actual recording */
static void finish_countin(void) {
    /* Snap playhead to beat boundary so recording metronome aligns with count-in grid.
     * Count-in snapped to the next beat, so playhead should be at a beat boundary. */
    if (g_samples_per_beat > 0) {
        int beat_pos = g_playhead % g_samples_per_beat;
        if (beat_pos != 0) {
            g_playhead += g_samples_per_beat - beat_pos;
        }
    }

    /* Reset count-in state */
    g_countin_counter = 0;
    g_countin_total_samples = 0;

    g_transport = TRANSPORT_RECORDING;
    ft_log("Count-in complete, recording at beat boundary");
}

static void toggle_recording(void) {
    if (g_transport == TRANSPORT_RECORDING) {
        /* Stop recording, switch to playback */
        g_transport = TRANSPORT_PLAYING;
        ft_log("Stopped recording");
    } else {
        start_recording();
    }
}

/* ============================================================================
 * Metronome
 * ============================================================================ */

static void update_metronome_timing(void) {
    /* samples_per_beat = sample_rate * 60 / bpm */
    g_samples_per_beat = (SAMPLE_RATE * 60) / g_tempo_bpm;
}

static void generate_metronome_click(int16_t *buffer, int frames) {
    /* Safety check - need valid samples_per_beat for beat calculations */
    if (g_samples_per_beat <= 0) {
        return;
    }

    /* No metronome when stopped */
    if (g_transport == TRANSPORT_STOPPED) {
        return;
    }

    for (int i = 0; i < frames; i++) {
        int beat_pos = -1;
        int should_click = 0;

        if (g_transport == TRANSPORT_COUNTIN) {
            /* Check if count-in is complete BEFORE generating click */
            if (g_countin_counter >= g_countin_total_samples) {
                finish_countin();
                /* Fall through to recording/playback handling below */
            } else {
                /* Still in count-in - use count-in counter */
                if (g_countin_counter >= 0) {
                    beat_pos = g_countin_counter % g_samples_per_beat;
                }
                should_click = 1;  /* Always click during count-in */
                g_countin_counter++;
            }
        }
        
        /* Handle playback/recording - derive beat position directly from playhead */
        if (g_transport == TRANSPORT_PLAYING || g_transport == TRANSPORT_RECORDING) {
            /* Beat position = (playhead + sample offset) % samples_per_beat
             * This ensures metronome is always aligned with the timeline */
            beat_pos = (g_playhead + i) % g_samples_per_beat;
            should_click = g_metronome_enabled;
        }

        /* Generate click at beat start (first 200 samples of each beat) */
        if (should_click && beat_pos >= 0 && beat_pos < 200) {
            float t = (float)beat_pos / 200.0f;
            float env = 1.0f - t;
            float click = sinf(beat_pos * 0.15f) * env * 0.3f;
            int16_t sample = (int16_t)(click * 32767.0f);

            int32_t l = buffer[i * 2] + sample;
            int32_t r = buffer[i * 2 + 1] + sample;
            if (l > 32767) l = 32767; if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; if (r < -32768) r = -32768;
            buffer[i * 2] = (int16_t)l;
            buffer[i * 2 + 1] = (int16_t)r;
        }
    }
}

/* ============================================================================
 * Plugin API Implementation
 * ============================================================================ */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    strncpy(g_module_dir, module_dir, MAX_PATH_LEN - 1);
    g_module_dir[MAX_PATH_LEN - 1] = '\0';

    ft_log("Four Track module loading...");

    /* Initialize subplugin host API */
    g_subplugin_host_api.api_version = MOVE_PLUGIN_API_VERSION;
    g_subplugin_host_api.sample_rate = SAMPLE_RATE;
    g_subplugin_host_api.frames_per_block = FRAMES_PER_BLOCK;
    g_subplugin_host_api.mapped_memory = g_host ? g_host->mapped_memory : NULL;
    g_subplugin_host_api.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    g_subplugin_host_api.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    g_subplugin_host_api.log = subplugin_log;
    g_subplugin_host_api.midi_send_internal = subplugin_midi_send_internal;
    g_subplugin_host_api.midi_send_external = subplugin_midi_send_external;

    /* Initialize tracks */
    init_tracks();

    /* Set default tempo */
    g_tempo_bpm = 120;
    update_metronome_timing();

    /* Scan for chain patches */
    scan_patches();

    /* Load Line In as default for all tracks */
    load_default_patches();

    ft_log("Four Track module loaded");
    return 0;
}

static void plugin_on_unload(void) {
    ft_log("Four Track module unloading...");

    /* Free track buffers and unload all synths */
    free_tracks();

    ft_log("Four Track module unloaded");
}

static void chain_panic_for_track(track_t *track) {
    /* Send all notes off on all channels via chain instance */
    if (!track->chain_plugin || !track->chain_instance) return;

    for (int ch = 0; ch < 16; ch++) {
        uint8_t msg[3] = {(uint8_t)(0xB0 | ch), 123, 0};  /* All notes off */
        track->chain_plugin->on_midi(track->chain_instance, msg, 3, MOVE_MIDI_SOURCE_INTERNAL);
    }
}

/* Panic all tracks */
static void chain_panic_all(void) {
    for (int i = 0; i < NUM_TRACKS; i++) {
        chain_panic_for_track(&g_tracks[i]);
    }
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    if (len < 1) return;

    track_t *target_track = NULL;

    /* Determine target track based on routing mode and MIDI source */
    if (source == MOVE_MIDI_SOURCE_INTERNAL) {
        /* Internal MIDI (pads, buttons) always goes to selected track */
        target_track = &g_tracks[g_selected_track];
    } else if (source == MOVE_MIDI_SOURCE_EXTERNAL) {
        /* External MIDI routing depends on mode */
        if (g_midi_routing_mode == MIDI_ROUTING_SPLIT_CHANNELS) {
            /* Split by MIDI channel: ch1 → track1, ch2 → track2, etc. */
            uint8_t status = msg[0];
            int channel = status & 0x0F;  /* Extract channel (0-15) */
            if (channel < NUM_TRACKS) {
                target_track = &g_tracks[channel];
            }
            /* Channels 4-15 are ignored in split mode */
        } else {
            /* Default: all external MIDI to selected track */
            target_track = &g_tracks[g_selected_track];
        }
    } else {
        /* Host-generated MIDI (clock, etc.) goes to selected track */
        target_track = &g_tracks[g_selected_track];
    }

    /* Forward MIDI to target track's chain instance */
    if (target_track && target_track->chain_plugin &&
        target_track->chain_instance && target_track->chain_plugin->on_midi) {
        target_track->chain_plugin->on_midi(target_track->chain_instance, msg, len, source);
    }
}

static void plugin_set_param(const char *key, const char *val) {
    char msg[256];

    if (strcmp(key, "select_track") == 0) {
        int track = atoi(val);
        if (track >= 0 && track < NUM_TRACKS) {
            g_selected_track = track;
            snprintf(msg, sizeof(msg), "Selected track %d", track + 1);
            ft_log(msg);
        }
    }
    else if (strcmp(key, "toggle_arm") == 0) {
        /* Toggle armed state on specified track (or selected if no value) */
        int track = (val && val[0]) ? atoi(val) : g_selected_track;
        if (track >= 0 && track < NUM_TRACKS) {
            g_tracks[track].armed = !g_tracks[track].armed;
            snprintf(msg, sizeof(msg), "Track %d %s", track + 1,
                     g_tracks[track].armed ? "armed" : "disarmed");
            ft_log(msg);
        }
    }
    else if (strcmp(key, "toggle_monitoring") == 0) {
        /* Toggle monitoring on specified track (or selected if no value) */
        int track = (val && val[0]) ? atoi(val) : g_selected_track;
        if (track >= 0 && track < NUM_TRACKS) {
            g_tracks[track].monitoring = !g_tracks[track].monitoring;
            snprintf(msg, sizeof(msg), "Track %d monitoring %s", track + 1,
                     g_tracks[track].monitoring ? "on" : "off");
            ft_log(msg);
        }
    }
    else if (strcmp(key, "track_level") == 0) {
        /* Format: "track:level" e.g., "0:0.8" */
        int track;
        float level;
        if (sscanf(val, "%d:%f", &track, &level) == 2) {
            if (track >= 0 && track < NUM_TRACKS) {
                g_tracks[track].level = level;
            }
        }
    }
    else if (strcmp(key, "track_pan") == 0) {
        int track;
        float pan;
        if (sscanf(val, "%d:%f", &track, &pan) == 2) {
            if (track >= 0 && track < NUM_TRACKS) {
                g_tracks[track].pan = pan;
            }
        }
    }
    else if (strcmp(key, "track_mute") == 0) {
        int track = atoi(val);
        if (track >= 0 && track < NUM_TRACKS) {
            g_tracks[track].muted = !g_tracks[track].muted;
        }
    }
    else if (strcmp(key, "track_solo") == 0) {
        int track = atoi(val);
        if (track >= 0 && track < NUM_TRACKS) {
            g_tracks[track].solo = !g_tracks[track].solo;
            update_solo_state();
        }
    }
    else if (strcmp(key, "clear_track") == 0) {
        int track = atoi(val);
        if (track >= 0 && track < NUM_TRACKS) {
            clear_track(track);
            snprintf(msg, sizeof(msg), "Cleared track %d", track + 1);
            ft_log(msg);
        }
    }
    else if (strcmp(key, "transport") == 0) {
        if (strcmp(val, "play") == 0) {
            start_playback();
        } else if (strcmp(val, "stop") == 0) {
            stop_transport();
        } else if (strcmp(val, "record") == 0) {
            toggle_recording();
        }
    }
    else if (strcmp(key, "goto_start") == 0) {
        g_playhead = 0;
        ft_log("Jumped to start");
    }
    else if (strcmp(key, "goto_end") == 0) {
        /* Jump to end of selected track's audio */
        if (g_selected_track >= 0 && g_selected_track < NUM_TRACKS) {
            int track_length = g_tracks[g_selected_track].length;
            if (track_length > 0) {
                /* length is in samples (stereo), playhead is in frames */
                g_playhead = track_length / NUM_CHANNELS;
            }
        }
        ft_log("Jumped to end of track");
    }
    else if (strcmp(key, "jump_bars") == 0) {
        /* Jump by N bars (positive = forward, negative = backward) */
        int bars = atoi(val);
        /* At 4/4 time signature: 1 bar = 4 beats */
        /* samples_per_bar = sample_rate * 60 / bpm * 4 */
        int samples_per_bar = (SAMPLE_RATE * 60 * 4) / g_tempo_bpm;
        int jump_samples = bars * samples_per_bar;
        g_playhead += jump_samples;
        if (g_playhead < 0) g_playhead = 0;
        snprintf(msg, sizeof(msg), "Jumped %d bars to %d", bars, g_playhead);
        ft_log(msg);
    }
    else if (strcmp(key, "tempo") == 0) {
        g_tempo_bpm = atoi(val);
        if (g_tempo_bpm < 20) g_tempo_bpm = 20;
        if (g_tempo_bpm > 300) g_tempo_bpm = 300;
        update_metronome_timing();
    }
    else if (strcmp(key, "metronome") == 0) {
        g_metronome_enabled = atoi(val);
    }
    else if (strcmp(key, "countin") == 0) {
        g_countin_enabled = atoi(val);
        ft_log(g_countin_enabled ? "Count-in enabled" : "Count-in disabled");
    }
    else if (strcmp(key, "midi_routing") == 0) {
        if (strcmp(val, "split") == 0) {
            g_midi_routing_mode = MIDI_ROUTING_SPLIT_CHANNELS;
            ft_log("MIDI routing: split by channel (ch1→T1, ch2→T2, etc.)");
        } else {
            g_midi_routing_mode = MIDI_ROUTING_SELECTED;
            ft_log("MIDI routing: all to selected track");
        }
    }
    else if (strcmp(key, "toggle_midi_routing") == 0) {
        /* Toggle between modes */
        if (g_midi_routing_mode == MIDI_ROUTING_SELECTED) {
            g_midi_routing_mode = MIDI_ROUTING_SPLIT_CHANNELS;
            ft_log("MIDI routing: split by channel (ch1→T1, ch2→T2, etc.)");
        } else {
            g_midi_routing_mode = MIDI_ROUTING_SELECTED;
            ft_log("MIDI routing: all to selected track");
        }
    }
    else if (strcmp(key, "loop_enabled") == 0) {
        g_loop_enabled = atoi(val);
    }
    else if (strcmp(key, "load_patch") == 0) {
        /* Load a chain patch for the selected track */
        int patch_idx = atoi(val);
        if (patch_idx >= 0 && patch_idx < g_patch_count) {
            track_t *track = &g_tracks[g_selected_track];

            /* Clear any previous error */
            g_last_error[0] = '\0';

            /* Ensure chain instance exists for this track */
            if (!track->chain_instance) {
                if (load_chain_for_track(track) != 0) {
                    snprintf(g_last_error, sizeof(g_last_error), "Failed to create chain instance");
                    snprintf(msg, sizeof(msg), "Track %d: failed to create chain instance",
                             g_selected_track + 1);
                    ft_log(msg);
                    return;
                }
            }

            /* Set patch name first (load_chain_patch_for_track uses it to find patch index) */
            strncpy(track->patch_name, g_patches[patch_idx].name, MAX_NAME_LEN - 1);

            /* Actually load and initialize the chain patch */
            int result = load_chain_patch_for_track(track, g_patches[patch_idx].path);
            if (result == 0) {
                /* Only set patch name/path on success */
                strncpy(track->patch_name, g_patches[patch_idx].name, MAX_NAME_LEN - 1);
                strncpy(track->patch_path, g_patches[patch_idx].path, MAX_PATH_LEN - 1);
                snprintf(msg, sizeof(msg), "Track %d: loaded patch '%s'",
                         g_selected_track + 1, g_patches[patch_idx].name);
            } else if (result == -2) {
                /* v1 plugin already in use - set error for UI */
                snprintf(g_last_error, sizeof(g_last_error),
                         "'%s' in use on another track", g_patches[patch_idx].name);
                snprintf(msg, sizeof(msg), "Track %d: '%s' already in use on another track (v1 plugin)",
                         g_selected_track + 1, g_patches[patch_idx].name);
            } else {
                snprintf(g_last_error, sizeof(g_last_error),
                         "Failed to load '%s'", g_patches[patch_idx].name);
                snprintf(msg, sizeof(msg), "Track %d: failed to load '%s'",
                         g_selected_track + 1, g_patches[patch_idx].name);
            }
            ft_log(msg);
        }
    }
    else if (strcmp(key, "clear_patch") == 0) {
        /* Clear patch from a track - unload entire chain and reload fresh */
        int track_idx = atoi(val);
        if (track_idx >= 0 && track_idx < NUM_TRACKS) {
            track_t *track = &g_tracks[track_idx];
            track->patch_name[0] = '\0';
            track->patch_path[0] = '\0';
            /* Panic and reload fresh chain (or could destroy/recreate instance) */
            chain_panic_for_track(track);
            track->chain_patch_idx = -1;
            snprintf(msg, sizeof(msg), "Track %d: patch cleared", track_idx + 1);
            ft_log(msg);
        }
    }
    else if (strcmp(key, "synth_param") == 0) {
        /* Forward parameter to selected track's chain (which routes to synth) */
        track_t *track = &g_tracks[g_selected_track];
        char *colon = strchr(val, ':');
        if (colon) {
            char pkey[64];
            int keylen = colon - val;
            if (keylen > 63) keylen = 63;
            strncpy(pkey, val, keylen);
            pkey[keylen] = '\0';

            /* Route via "synth:" prefix to chain */
            char chain_key[80];
            snprintf(chain_key, sizeof(chain_key), "synth:%s", pkey);
            if (track->chain_plugin && track->chain_instance && track->chain_plugin->set_param) {
                track->chain_plugin->set_param(track->chain_instance, chain_key, colon + 1);
            }
        }
    }
    else if (strcmp(key, "rescan_patches") == 0) {
        scan_patches();
    }
    else if (strcmp(key, "clear_error") == 0) {
        g_last_error[0] = '\0';
    }
    else if (strcmp(key, "toggle_mute") == 0) {
        int track = atoi(val);
        if (track >= 0 && track < NUM_TRACKS) {
            g_tracks[track].muted = !g_tracks[track].muted;
            snprintf(msg, sizeof(msg), "Track %d %s", track + 1,
                     g_tracks[track].muted ? "muted" : "unmuted");
            ft_log(msg);
        }
    }
    else if (strcmp(key, "record_seconds") == 0) {
        int secs = atoi(val);
        if (secs >= 10 && secs <= MAX_RECORD_SECONDS) {
            g_record_seconds = secs;
            snprintf(msg, sizeof(msg), "Record time limit set to %d seconds", secs);
            ft_log(msg);
        }
    }
}

static int plugin_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "selected_track") == 0) {
        return snprintf(buf, buf_len, "%d", g_selected_track);
    }
    else if (strcmp(key, "any_armed") == 0) {
        return snprintf(buf, buf_len, "%d", any_track_armed());
    }
    else if (strcmp(key, "transport") == 0) {
        const char *state;
        switch (g_transport) {
            case TRANSPORT_STOPPED:   state = "stopped"; break;
            case TRANSPORT_PLAYING:   state = "playing"; break;
            case TRANSPORT_RECORDING: state = "recording"; break;
            case TRANSPORT_COUNTIN:   state = "countin"; break;
            default:                  state = "unknown"; break;
        }
        return snprintf(buf, buf_len, "%s", state);
    }
    else if (strcmp(key, "countin") == 0) {
        return snprintf(buf, buf_len, "%d", g_countin_enabled);
    }
    else if (strcmp(key, "countin_beats") == 0) {
        /* Calculate beats remaining from counter */
        int beats_remaining = 0;
        if (g_transport == TRANSPORT_COUNTIN && g_samples_per_beat > 0) {
            int remaining_samples = g_countin_total_samples - g_countin_counter;
            if (remaining_samples > 0) {
                beats_remaining = (remaining_samples + g_samples_per_beat - 1) / g_samples_per_beat;
            }
        }
        return snprintf(buf, buf_len, "%d", beats_remaining);
    }
    else if (strcmp(key, "tempo") == 0) {
        return snprintf(buf, buf_len, "%d", g_tempo_bpm);
    }
    else if (strcmp(key, "metronome") == 0) {
        return snprintf(buf, buf_len, "%d", g_metronome_enabled);
    }
    else if (strcmp(key, "midi_routing") == 0) {
        return snprintf(buf, buf_len, "%s",
                        g_midi_routing_mode == MIDI_ROUTING_SPLIT_CHANNELS ? "split" : "selected");
    }
    else if (strcmp(key, "loop_enabled") == 0) {
        return snprintf(buf, buf_len, "%d", g_loop_enabled);
    }
    else if (strcmp(key, "playhead") == 0) {
        return snprintf(buf, buf_len, "%d", g_playhead / (SAMPLE_RATE / 1000));  /* In ms */
    }
    else if (strcmp(key, "patch_count") == 0) {
        return snprintf(buf, buf_len, "%d", g_patch_count);
    }
    else if (strncmp(key, "patch_name_", 11) == 0) {
        int idx = atoi(key + 11);
        if (idx >= 0 && idx < g_patch_count) {
            return snprintf(buf, buf_len, "%s", g_patches[idx].name);
        }
        return -1;
    }
    else if (strncmp(key, "track_", 6) == 0) {
        /* track_N_param format */
        int track;
        char param[32];
        if (sscanf(key + 6, "%d_%31s", &track, param) == 2) {
            if (track >= 0 && track < NUM_TRACKS) {
                if (strcmp(param, "level") == 0) {
                    return snprintf(buf, buf_len, "%.2f", g_tracks[track].level);
                }
                else if (strcmp(param, "pan") == 0) {
                    return snprintf(buf, buf_len, "%.2f", g_tracks[track].pan);
                }
                else if (strcmp(param, "muted") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].muted);
                }
                else if (strcmp(param, "solo") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].solo);
                }
                else if (strcmp(param, "length") == 0) {
                    /* Return length in seconds */
                    float secs = (float)g_tracks[track].length / (SAMPLE_RATE * NUM_CHANNELS);
                    return snprintf(buf, buf_len, "%.1f", secs);
                }
                else if (strcmp(param, "patch") == 0) {
                    return snprintf(buf, buf_len, "%s",
                                    g_tracks[track].patch_name[0] ? g_tracks[track].patch_name : "Empty");
                }
                else if (strcmp(param, "armed") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].armed);
                }
                else if (strcmp(param, "monitoring") == 0) {
                    return snprintf(buf, buf_len, "%d", g_tracks[track].monitoring);
                }
                else if (strcmp(param, "synth_loaded") == 0) {
                    /* Check if chain instance has a patch loaded */
                    int loaded = (g_tracks[track].chain_instance != NULL && g_tracks[track].chain_patch_idx >= 0);
                    return snprintf(buf, buf_len, "%d", loaded);
                }
            }
        }
    }
    else if (strcmp(key, "synth_loaded") == 0) {
        /* Check if selected track has a chain with patch loaded */
        track_t *track = &g_tracks[g_selected_track];
        int loaded = (track->chain_instance != NULL && track->chain_patch_idx >= 0);
        return snprintf(buf, buf_len, "%d", loaded);
    }
    else if (strcmp(key, "record_seconds") == 0) {
        return snprintf(buf, buf_len, "%d", g_record_seconds);
    }
    else if (strcmp(key, "max_record_seconds") == 0) {
        return snprintf(buf, buf_len, "%d", MAX_RECORD_SECONDS);
    }
    else if (strcmp(key, "knob_mapping_count") == 0 || strncmp(key, "knob_", 5) == 0) {
        /* Delegate knob queries to the chain instance for selected track */
        track_t *track = &g_tracks[g_selected_track];
        if (track->chain_plugin && track->chain_instance && track->chain_plugin->get_param) {
            return track->chain_plugin->get_param(track->chain_instance, key, buf, buf_len);
        }
        return -1;  /* No chain loaded */
    }
    else if (strcmp(key, "last_error") == 0) {
        return snprintf(buf, buf_len, "%s", g_last_error);
    }

    return -1;
}

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    int16_t chain_buffers[NUM_TRACKS][FRAMES_PER_BLOCK * 2];
    int32_t mix_buffer[FRAMES_PER_BLOCK * 2];

    /* Clear mix buffer */
    memset(mix_buffer, 0, sizeof(mix_buffer));

    /* Render each track's chain (synth + audio FX) */
    for (int t = 0; t < NUM_TRACKS; t++) {
        memset(chain_buffers[t], 0, sizeof(chain_buffers[t]));
        track_t *track = &g_tracks[t];
        if (track->chain_plugin && track->chain_instance && track->chain_plugin->render_block) {
            track->chain_plugin->render_block(track->chain_instance, chain_buffers[t], frames);
        }
    }

    /* Process each track */
    for (int t = 0; t < NUM_TRACKS; t++) {
        track_t *track = &g_tracks[t];

        /* Recording: write track's chain output to its buffer if armed */
        if (g_transport == TRANSPORT_RECORDING && track->armed) {
            int write_pos = g_playhead * NUM_CHANNELS;
            int max_samples = g_record_seconds * SAMPLE_RATE * NUM_CHANNELS;
            for (int i = 0; i < frames && write_pos < max_samples - 1; i++) {
                track->buffer[write_pos] = chain_buffers[t][i * 2];
                track->buffer[write_pos + 1] = chain_buffers[t][i * 2 + 1];
                write_pos += 2;
            }
            /* Update track length */
            int new_length = (g_playhead + frames) * NUM_CHANNELS;
            if (new_length > track->length && new_length <= max_samples) {
                track->length = new_length;
            }
        }

        /* Skip mixing if muted (unless in solo mode and not soloed) */
        if (track->muted) continue;
        if (g_any_solo && !track->solo) continue;

        /* Playback: mix track audio into output (skip during count-in and for track being recorded) */
        int is_recording_this_track = (g_transport == TRANSPORT_RECORDING && track->armed);
        int is_playing_back = (g_transport == TRANSPORT_PLAYING || g_transport == TRANSPORT_RECORDING);
        if (track->length > 0 && is_playing_back && !is_recording_this_track) {
            int read_pos = g_playhead * NUM_CHANNELS;

            for (int i = 0; i < frames; i++) {
                /* Check bounds */
                if (read_pos >= track->length) {
                    if (g_loop_enabled && g_loop_end > 0) {
                        read_pos = g_loop_start * NUM_CHANNELS;
                    } else {
                        break;
                    }
                }

                /* Read samples */
                int16_t l = track->buffer[read_pos];
                int16_t r = track->buffer[read_pos + 1];

                /* Apply level and pan */
                float level = track->level;
                float pan = track->pan;  /* -1 to +1 */
                float pan_l = (pan < 0) ? 1.0f : 1.0f - pan;
                float pan_r = (pan > 0) ? 1.0f : 1.0f + pan;

                int32_t out_l = (int32_t)(l * level * pan_l);
                int32_t out_r = (int32_t)(r * level * pan_r);

                /* Mix */
                mix_buffer[i * 2] += out_l;
                mix_buffer[i * 2 + 1] += out_r;

                read_pos += 2;
            }
        }

        /* Monitor live chain output for this track if monitoring is enabled */
        int has_chain = (track->chain_instance != NULL && track->chain_patch_idx >= 0);
        if (track->monitoring && has_chain) {
            float level = track->level;
            float pan = track->pan;
            float pan_l = (pan < 0) ? 1.0f : 1.0f - pan;
            float pan_r = (pan > 0) ? 1.0f : 1.0f + pan;

            for (int i = 0; i < frames; i++) {
                int32_t l = (int32_t)(chain_buffers[t][i * 2] * level * pan_l);
                int32_t r = (int32_t)(chain_buffers[t][i * 2 + 1] * level * pan_r);
                mix_buffer[i * 2] += l;
                mix_buffer[i * 2 + 1] += r;
            }
        }
    }

    /* Advance playhead (but not during count-in - playhead stays put) */
    if (g_transport == TRANSPORT_PLAYING || g_transport == TRANSPORT_RECORDING) {
        g_playhead += frames;

        /* Loop handling */
        if (g_loop_enabled && g_loop_end > 0 && g_playhead >= g_loop_end) {
            g_playhead = g_loop_start;
        }
    }

    /* Add metronome if enabled */
    int16_t click_buffer[FRAMES_PER_BLOCK * 2];
    memset(click_buffer, 0, sizeof(click_buffer));
    generate_metronome_click(click_buffer, frames);
    for (int i = 0; i < frames * 2; i++) {
        mix_buffer[i] += click_buffer[i];
    }

    /* Final output with clipping */
    for (int i = 0; i < frames * 2; i++) {
        int32_t sample = mix_buffer[i];
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        out_interleaved_lr[i] = (int16_t)sample;
    }
}

/* ============================================================================
 * Plugin Entry Point
 * ============================================================================ */

static plugin_api_v1_t g_plugin_api = {
    .api_version = MOVE_PLUGIN_API_VERSION,
    .on_load = plugin_on_load,
    .on_unload = plugin_on_unload,
    .on_midi = plugin_on_midi,
    .set_param = plugin_set_param,
    .get_param = plugin_get_param,
    .render_block = plugin_render_block
};

plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host) {
    g_host = host;
    return &g_plugin_api;
}
