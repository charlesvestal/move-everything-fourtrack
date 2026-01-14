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

/* ============================================================================
 * Constants
 * ============================================================================ */

#define NUM_TRACKS 4
#define SAMPLE_RATE 44100
#define FRAMES_PER_BLOCK 128
#define NUM_CHANNELS 2

/* Recording buffer: configurable seconds per track at 44.1kHz stereo
 * Memory usage: ~176KB per second per track (stereo int16)
 * 120s × 4 tracks = ~84MB, 300s × 4 = ~210MB
 */
#define DEFAULT_RECORD_SECONDS 120
#define MAX_RECORD_SECONDS 300  /* 5 minutes max per track */
#define MAX_RECORD_SAMPLES (MAX_RECORD_SECONDS * SAMPLE_RATE)
#define TRACK_BUFFER_SIZE (MAX_RECORD_SAMPLES * NUM_CHANNELS)

static int g_record_seconds = DEFAULT_RECORD_SECONDS;

/* Path limits */
#define MAX_PATH_LEN 512
#define MAX_NAME_LEN 64
#define MAX_PATCHES 64

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
    /* Per-track synth */
    void *synth_handle;
    plugin_api_v1_t *synth_plugin;
    char synth_module[MAX_NAME_LEN];
    /* Per-track knob mappings */
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
    TRANSPORT_RECORDING
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

/* Metronome */
static int g_metronome_enabled = 0;
static int g_tempo_bpm = 120;
static int g_samples_per_beat = 0;
static int g_metronome_counter = 0;

/* Project file */
static char g_project_path[MAX_PATH_LEN];
static int g_project_loaded = 0;

/* Solo tracking */
static int g_any_solo = 0;

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

static int load_synth_for_track(track_t *track, const char *module_path) {
    char msg[256];
    char dsp_path[MAX_PATH_LEN];

    /* Build path to dsp.so */
    snprintf(dsp_path, sizeof(dsp_path), "%s/dsp.so", module_path);

    snprintf(msg, sizeof(msg), "Loading synth from: %s", dsp_path);
    ft_log(msg);

    /* Open the shared library */
    track->synth_handle = dlopen(dsp_path, RTLD_NOW | RTLD_LOCAL);
    if (!track->synth_handle) {
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
        ft_log(msg);
        return -1;
    }

    /* Get init function */
    move_plugin_init_v1_fn init_fn = (move_plugin_init_v1_fn)dlsym(track->synth_handle, MOVE_PLUGIN_INIT_SYMBOL);
    if (!init_fn) {
        snprintf(msg, sizeof(msg), "dlsym failed: %s", dlerror());
        ft_log(msg);
        dlclose(track->synth_handle);
        track->synth_handle = NULL;
        return -1;
    }

    /* Initialize sub-plugin */
    track->synth_plugin = init_fn(&g_subplugin_host_api);
    if (!track->synth_plugin) {
        ft_log("Synth plugin init returned NULL");
        dlclose(track->synth_handle);
        track->synth_handle = NULL;
        return -1;
    }

    /* Call on_load */
    if (track->synth_plugin->on_load) {
        int ret = track->synth_plugin->on_load(module_path, NULL);
        if (ret != 0) {
            snprintf(msg, sizeof(msg), "Synth on_load failed: %d", ret);
            ft_log(msg);
            dlclose(track->synth_handle);
            track->synth_handle = NULL;
            track->synth_plugin = NULL;
            return -1;
        }
    }

    ft_log("Synth loaded successfully");
    return 0;
}

static void unload_synth_for_track(track_t *track) {
    if (track->synth_plugin && track->synth_plugin->on_unload) {
        track->synth_plugin->on_unload();
    }
    if (track->synth_handle) {
        dlclose(track->synth_handle);
    }
    track->synth_handle = NULL;
    track->synth_plugin = NULL;
    track->synth_module[0] = '\0';
}

/* Forward declarations */
static void synth_panic_for_track(track_t *track);

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

/* Parse a chain patch file and load the synth */
static int load_chain_patch_for_track(track_t *track, const char *patch_path) {
    char msg[256];

    FILE *f = fopen(patch_path, "r");
    if (!f) {
        snprintf(msg, sizeof(msg), "Failed to open patch: %s", patch_path);
        ft_log(msg);
        return -1;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size > 4096) {
        fclose(f);
        ft_log("Patch file too large");
        return -1;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);

    /* Extract synth module name */
    char synth_module[MAX_NAME_LEN] = "";
    if (json_get_string_in_section(json, "synth", "module", synth_module, MAX_NAME_LEN) != 0) {
        /* Try old format or default */
        if (json_get_string(json, "synth", synth_module, MAX_NAME_LEN) != 0) {
            strcpy(synth_module, "sf2");  /* Default */
        }
    }

    /* Extract preset number */
    int preset = 0;
    json_get_int_in_section(json, "synth", "preset", &preset);

    snprintf(msg, sizeof(msg), "Patch synth: %s, preset: %d", synth_module, preset);
    ft_log(msg);

    /* Parse knob mappings before freeing JSON */
    parse_knob_mappings(track, json);

    free(json);

    /* Build path to synth module */
    char synth_path[MAX_PATH_LEN];

    /* Check if it's an internal sound generator (linein) */
    if (strcmp(synth_module, "linein") == 0) {
        /* Internal - in chain/sound_generators/ */
        strncpy(synth_path, g_module_dir, sizeof(synth_path) - 1);
        char *last_slash = strrchr(synth_path, '/');
        if (last_slash) {
            snprintf(last_slash + 1, sizeof(synth_path) - (last_slash - synth_path) - 1,
                     "chain/sound_generators/%s", synth_module);
        }
    } else {
        /* External module (sf2, dx7, etc.) - in modules/ */
        strncpy(synth_path, g_module_dir, sizeof(synth_path) - 1);
        char *last_slash = strrchr(synth_path, '/');
        if (last_slash) {
            snprintf(last_slash + 1, sizeof(synth_path) - (last_slash - synth_path) - 1,
                     "%s", synth_module);
        }
    }

    snprintf(msg, sizeof(msg), "Loading synth from: %s", synth_path);
    ft_log(msg);

    /* Unload current synth for this track and load new one */
    synth_panic_for_track(track);
    unload_synth_for_track(track);

    if (load_synth_for_track(track, synth_path) != 0) {
        snprintf(msg, sizeof(msg), "Failed to load synth: %s", synth_module);
        ft_log(msg);
        return -1;
    }

    strncpy(track->synth_module, synth_module, MAX_NAME_LEN - 1);

    /* Set preset */
    if (track->synth_plugin && track->synth_plugin->set_param) {
        char preset_str[16];
        snprintf(preset_str, sizeof(preset_str), "%d", preset);
        track->synth_plugin->set_param("preset", preset_str);
    }

    return 0;
}

/* ============================================================================
 * Patch Scanning
 * ============================================================================ */

static void scan_patches(void) {
    char patches_dir[MAX_PATH_LEN];
    char msg[256];

    /* Build path to chain patches directory */
    /* Go up from fourtrack to modules, then to chain/patches */
    strncpy(patches_dir, g_module_dir, sizeof(patches_dir) - 1);
    char *last_slash = strrchr(patches_dir, '/');
    if (last_slash) {
        snprintf(last_slash + 1, sizeof(patches_dir) - (last_slash - patches_dir) - 1,
                 "chain/patches");
    } else {
        strncpy(patches_dir, "modules/chain/patches", sizeof(patches_dir) - 1);
    }

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
        strncpy(g_tracks[i].patch_name, g_patches[linein_idx].name, MAX_NAME_LEN - 1);
        strncpy(g_tracks[i].patch_path, g_patches[linein_idx].path, MAX_PATH_LEN - 1);
        /* Load synth for each track */
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
        g_tracks[i].monitoring = 1;  /* Monitoring on by default */
        g_tracks[i].patch_name[0] = '\0';
        g_tracks[i].patch_path[0] = '\0';
        g_tracks[i].synth_handle = NULL;
        g_tracks[i].synth_plugin = NULL;
        g_tracks[i].synth_module[0] = '\0';
        g_tracks[i].knob_mapping_count = 0;
    }
}

static void free_tracks(void) {
    for (int i = 0; i < NUM_TRACKS; i++) {
        /* Unload synth for this track */
        unload_synth_for_track(&g_tracks[i]);
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

    /* Clear all armed tracks we're about to record to */
    for (int i = 0; i < NUM_TRACKS; i++) {
        if (g_tracks[i].armed) {
            clear_track(i);
        }
    }
    g_transport = TRANSPORT_RECORDING;

    ft_log("Recording started");
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
    if (!g_metronome_enabled || g_transport == TRANSPORT_STOPPED) {
        return;
    }

    for (int i = 0; i < frames; i++) {
        if (g_metronome_counter >= g_samples_per_beat) {
            g_metronome_counter = 0;
        }

        /* Generate a short click at the start of each beat */
        if (g_metronome_counter < 200) {
            /* Simple decaying sine wave click */
            float t = (float)g_metronome_counter / 200.0f;
            float env = 1.0f - t;
            float click = sinf(g_metronome_counter * 0.15f) * env * 0.3f;
            int16_t sample = (int16_t)(click * 32767.0f);

            /* Mix into buffer (stereo) */
            int32_t l = buffer[i * 2] + sample;
            int32_t r = buffer[i * 2 + 1] + sample;

            /* Clamp */
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;

            buffer[i * 2] = (int16_t)l;
            buffer[i * 2 + 1] = (int16_t)r;
        }

        g_metronome_counter++;
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

static void synth_panic_for_track(track_t *track) {
    if (!track->synth_plugin || !track->synth_plugin->on_midi) return;

    /* Send all notes off on all channels */
    for (int ch = 0; ch < 16; ch++) {
        uint8_t msg[3] = {(uint8_t)(0xB0 | ch), 123, 0};  /* All notes off */
        track->synth_plugin->on_midi(msg, 3, MOVE_MIDI_SOURCE_INTERNAL);
    }
}

/* Panic all tracks */
static void synth_panic_all(void) {
    for (int i = 0; i < NUM_TRACKS; i++) {
        synth_panic_for_track(&g_tracks[i]);
    }
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    if (len < 1) return;

    /* Get selected track for knob mappings and MIDI routing */
    track_t *track = &g_tracks[g_selected_track];

    /* Handle knob CC mappings (CC 71-78) - relative encoders */
    if (len >= 3 && (msg[0] & 0xF0) == 0xB0) {
        uint8_t cc = msg[1];
        if (cc >= KNOB_CC_START && cc <= KNOB_CC_END) {
            for (int i = 0; i < track->knob_mapping_count; i++) {
                if (track->knob_mappings[i].cc == cc) {
                    /* Relative encoder: 1 = increment, 127 = decrement */
                    int is_int = (track->knob_mappings[i].type == KNOB_TYPE_INT);
                    float delta = 0.0f;
                    if (msg[2] == 1) {
                        delta = is_int ? (float)KNOB_STEP_INT : KNOB_STEP_FLOAT;
                    } else if (msg[2] == 127) {
                        delta = is_int ? (float)(-KNOB_STEP_INT) : -KNOB_STEP_FLOAT;
                    } else {
                        /* For larger increments/decrements */
                        if (msg[2] < 64) {
                            delta = (is_int ? (float)KNOB_STEP_INT : KNOB_STEP_FLOAT) * msg[2];
                        } else {
                            delta = (is_int ? (float)(-KNOB_STEP_INT) : -KNOB_STEP_FLOAT) * (128 - msg[2]);
                        }
                    }

                    /* Update current value with min/max clamping */
                    float new_val = track->knob_mappings[i].current_value + delta;
                    if (new_val < track->knob_mappings[i].min_val) new_val = track->knob_mappings[i].min_val;
                    if (new_val > track->knob_mappings[i].max_val) new_val = track->knob_mappings[i].max_val;
                    if (is_int) new_val = (float)((int)new_val);  /* Round to int */
                    track->knob_mappings[i].current_value = new_val;

                    /* Convert to string for set_param */
                    char val_str[16];
                    if (is_int) {
                        snprintf(val_str, sizeof(val_str), "%d", (int)new_val);
                    } else {
                        snprintf(val_str, sizeof(val_str), "%.3f", new_val);
                    }

                    /* Route to track's synth (we only support synth target for now) */
                    if (track->synth_plugin && track->synth_plugin->set_param) {
                        track->synth_plugin->set_param(track->knob_mappings[i].param, val_str);
                    }
                    return;  /* CC handled */
                }
            }
        }
    }

    /* Forward MIDI to selected track's synth */
    if (track->synth_plugin && track->synth_plugin->on_midi) {
        track->synth_plugin->on_midi(msg, len, source);
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
    else if (strcmp(key, "loop_enabled") == 0) {
        g_loop_enabled = atoi(val);
    }
    else if (strcmp(key, "load_patch") == 0) {
        /* Load a chain patch for the selected track */
        int patch_idx = atoi(val);
        if (patch_idx >= 0 && patch_idx < g_patch_count) {
            track_t *track = &g_tracks[g_selected_track];
            strncpy(track->patch_name, g_patches[patch_idx].name, MAX_NAME_LEN - 1);
            strncpy(track->patch_path, g_patches[patch_idx].path, MAX_PATH_LEN - 1);

            /* Actually load and initialize the synth from the patch */
            if (load_chain_patch_for_track(track, g_patches[patch_idx].path) == 0) {
                snprintf(msg, sizeof(msg), "Track %d: loaded patch '%s'",
                         g_selected_track + 1, g_patches[patch_idx].name);
            } else {
                snprintf(msg, sizeof(msg), "Track %d: failed to load '%s'",
                         g_selected_track + 1, g_patches[patch_idx].name);
            }
            ft_log(msg);
        }
    }
    else if (strcmp(key, "clear_patch") == 0) {
        /* Clear patch from a track */
        int track_idx = atoi(val);
        if (track_idx >= 0 && track_idx < NUM_TRACKS) {
            track_t *track = &g_tracks[track_idx];
            track->patch_name[0] = '\0';
            track->patch_path[0] = '\0';
            /* Unload the synth for this track */
            synth_panic_for_track(track);
            unload_synth_for_track(track);
            snprintf(msg, sizeof(msg), "Track %d: patch cleared", track_idx + 1);
            ft_log(msg);
        }
    }
    else if (strcmp(key, "synth_param") == 0) {
        /* Forward parameter to selected track's synth "key:val" */
        track_t *track = &g_tracks[g_selected_track];
        if (track->synth_plugin && track->synth_plugin->set_param) {
            char *colon = strchr(val, ':');
            if (colon) {
                char pkey[64];
                int keylen = colon - val;
                if (keylen > 63) keylen = 63;
                strncpy(pkey, val, keylen);
                pkey[keylen] = '\0';
                track->synth_plugin->set_param(pkey, colon + 1);
            }
        }
    }
    else if (strcmp(key, "rescan_patches") == 0) {
        scan_patches();
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
            default:                  state = "unknown"; break;
        }
        return snprintf(buf, buf_len, "%s", state);
    }
    else if (strcmp(key, "tempo") == 0) {
        return snprintf(buf, buf_len, "%d", g_tempo_bpm);
    }
    else if (strcmp(key, "metronome") == 0) {
        return snprintf(buf, buf_len, "%d", g_metronome_enabled);
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
                    return snprintf(buf, buf_len, "%d", g_tracks[track].synth_plugin ? 1 : 0);
                }
            }
        }
    }
    else if (strcmp(key, "synth_loaded") == 0) {
        /* Check if selected track has a synth loaded */
        return snprintf(buf, buf_len, "%d", g_tracks[g_selected_track].synth_plugin ? 1 : 0);
    }
    else if (strcmp(key, "record_seconds") == 0) {
        return snprintf(buf, buf_len, "%d", g_record_seconds);
    }
    else if (strcmp(key, "max_record_seconds") == 0) {
        return snprintf(buf, buf_len, "%d", MAX_RECORD_SECONDS);
    }
    else if (strcmp(key, "knob_mapping_count") == 0) {
        /* Return knob mapping count for selected track */
        return snprintf(buf, buf_len, "%d", g_tracks[g_selected_track].knob_mapping_count);
    }
    else if (strncmp(key, "knob_", 5) == 0) {
        /* knob_N_param format (N is 1-8 for knobs, mapping to CC 71-78) */
        int knob_num;
        char param[32];
        if (sscanf(key + 5, "%d_%31s", &knob_num, param) == 2) {
            /* Find mapping for this knob (CC = 70 + knob_num) in selected track */
            track_t *track = &g_tracks[g_selected_track];
            int cc = 70 + knob_num;
            for (int i = 0; i < track->knob_mapping_count; i++) {
                if (track->knob_mappings[i].cc == cc) {
                    if (strcmp(param, "name") == 0) {
                        return snprintf(buf, buf_len, "%s", track->knob_mappings[i].name);
                    }
                    else if (strcmp(param, "value") == 0) {
                        if (track->knob_mappings[i].type == KNOB_TYPE_INT) {
                            return snprintf(buf, buf_len, "%d", (int)track->knob_mappings[i].current_value);
                        } else {
                            return snprintf(buf, buf_len, "%.2f", track->knob_mappings[i].current_value);
                        }
                    }
                    else if (strcmp(param, "min") == 0) {
                        return snprintf(buf, buf_len, "%.2f", track->knob_mappings[i].min_val);
                    }
                    else if (strcmp(param, "max") == 0) {
                        return snprintf(buf, buf_len, "%.2f", track->knob_mappings[i].max_val);
                    }
                    else if (strcmp(param, "type") == 0) {
                        return snprintf(buf, buf_len, "%s",
                                        track->knob_mappings[i].type == KNOB_TYPE_INT ? "int" : "float");
                    }
                    else if (strcmp(param, "percent") == 0) {
                        float range = track->knob_mappings[i].max_val - track->knob_mappings[i].min_val;
                        float pct = 0;
                        if (range > 0) {
                            pct = (track->knob_mappings[i].current_value - track->knob_mappings[i].min_val) / range * 100.0f;
                        }
                        return snprintf(buf, buf_len, "%d", (int)pct);
                    }
                    break;
                }
            }
        }
        return -1;  /* Knob not mapped */
    }

    return -1;
}

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    int16_t synth_buffers[NUM_TRACKS][FRAMES_PER_BLOCK * 2];
    int32_t mix_buffer[FRAMES_PER_BLOCK * 2];

    /* Clear mix buffer */
    memset(mix_buffer, 0, sizeof(mix_buffer));

    /* Render each track's synth */
    for (int t = 0; t < NUM_TRACKS; t++) {
        memset(synth_buffers[t], 0, sizeof(synth_buffers[t]));
        track_t *track = &g_tracks[t];
        if (track->synth_plugin && track->synth_plugin->render_block) {
            track->synth_plugin->render_block(synth_buffers[t], frames);
        }
    }

    /* Process each track */
    for (int t = 0; t < NUM_TRACKS; t++) {
        track_t *track = &g_tracks[t];

        /* Recording: write track's synth output to its buffer if armed */
        if (g_transport == TRANSPORT_RECORDING && track->armed) {
            int write_pos = g_playhead * NUM_CHANNELS;
            int max_samples = g_record_seconds * SAMPLE_RATE * NUM_CHANNELS;
            for (int i = 0; i < frames && write_pos < max_samples - 1; i++) {
                track->buffer[write_pos] = synth_buffers[t][i * 2];
                track->buffer[write_pos + 1] = synth_buffers[t][i * 2 + 1];
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

        /* Playback: mix track audio into output (skip track being recorded) */
        int is_recording_this_track = (g_transport == TRANSPORT_RECORDING && track->armed);
        if (track->length > 0 && g_transport != TRANSPORT_STOPPED && !is_recording_this_track) {
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

        /* Monitor live synth output for this track if monitoring is enabled */
        if (track->monitoring && track->synth_plugin) {
            float level = track->level;
            float pan = track->pan;
            float pan_l = (pan < 0) ? 1.0f : 1.0f - pan;
            float pan_r = (pan > 0) ? 1.0f : 1.0f + pan;

            for (int i = 0; i < frames; i++) {
                int32_t l = (int32_t)(synth_buffers[t][i * 2] * level * pan_l);
                int32_t r = (int32_t)(synth_buffers[t][i * 2 + 1] * level * pan_r);
                mix_buffer[i * 2] += l;
                mix_buffer[i * 2 + 1] += r;
            }
        }
    }

    /* Advance playhead */
    if (g_transport != TRANSPORT_STOPPED) {
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
