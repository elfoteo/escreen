#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include "escreen.h"

// --- Parser Helpers ---

static void parse_color(const char *hex, escreen_color_t *color) {
	if (hex[0] == '#') hex++;
	unsigned int r, g, b, a = 255;
	size_t len = strlen(hex);
	if (len >= 8) {
		if (sscanf(hex, "%02x%02x%02x%02x", &r, &g, &b, &a) >= 4) {
			color->r = r / 255.0; color->g = g / 255.0; color->b = b / 255.0; color->a = a / 255.0;
		}
	} else if (len >= 6) {
		if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) >= 3) {
			color->r = r / 255.0; color->g = g / 255.0; color->b = b / 255.0; color->a = 1.0;
		}
	}
}

static void color_to_hex(escreen_color_t c, char *buf) {
	snprintf(buf, 10, "#%02X%02X%02X%02X", 
		(int)(c.r * 255 + 0.5), (int)(c.g * 255 + 0.5), (int)(c.b * 255 + 0.5), (int)(c.a * 255 + 0.5));
}

static char *expand_tilde(const char *path) {
	if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
		const char *home = getenv("HOME");
		if (home) {
			size_t home_len = strlen(home);
			size_t path_len = strlen(path);
			char *expanded = malloc(home_len + path_len);
			if (expanded) {
				strcpy(expanded, home);
				strcat(expanded, path + 1);
				return expanded;
			}
		}
	}
	return strdup(path);
}

// --- Modular Config Framework ---

typedef struct {
	const char *key;
	const char *description;
	void (*parse)(struct escreen_state *state, const char *val);
	void (*format)(struct escreen_state *state, FILE *f);
	bool seen;
} config_entry_t;

static void parse_auto_save_enabled(struct escreen_state *state, const char *val) {
	state->config.auto_save_enabled = (strcmp(val, "true") == 0);
}
static void format_auto_save_enabled(struct escreen_state *state, FILE *f) {
	fprintf(f, "auto_save_enabled=%s\n", state->config.auto_save_enabled ? "true" : "false");
}

static void parse_auto_save_path(struct escreen_state *state, const char *val) {
	free(state->config.auto_save_path);
	state->config.auto_save_path = expand_tilde(val);
}
static void format_auto_save_path(struct escreen_state *state, FILE *f) {
	fprintf(f, "auto_save_path=%s\n", state->config.auto_save_path);
}

static void parse_auto_save_format(struct escreen_state *state, const char *val) {
	free(state->config.auto_save_format);
	state->config.auto_save_format = strdup(val);
}
static void format_auto_save_format(struct escreen_state *state, FILE *f) {
	fprintf(f, "auto_save_format=%s\n", state->config.auto_save_format);
}

static void parse_auto_save_filename_format(struct escreen_state *state, const char *val) {
	free(state->config.auto_save_filename_format);
	state->config.auto_save_filename_format = strdup(val);
}
static void format_auto_save_filename_format(struct escreen_state *state, FILE *f) {
	fprintf(f, "auto_save_filename_format=%s\n", state->config.auto_save_filename_format);
}

static void parse_color_accent(struct escreen_state *state, const char *val) {
	parse_color(val, &state->config.colors.accent);
}
static void format_color_accent(struct escreen_state *state, FILE *f) {
	char hex[10]; color_to_hex(state->config.colors.accent, hex);
	fprintf(f, "color_accent=%s\n", hex);
}

static void parse_color_toolbar_bg(struct escreen_state *state, const char *val) {
	parse_color(val, &state->config.colors.toolbar_bg);
}
static void format_color_toolbar_bg(struct escreen_state *state, FILE *f) {
	char hex[10]; color_to_hex(state->config.colors.toolbar_bg, hex);
	fprintf(f, "color_toolbar_bg=%s\n", hex);
}

static void parse_color_button_hover(struct escreen_state *state, const char *val) {
	parse_color(val, &state->config.colors.button_hover);
}
static void format_color_button_hover(struct escreen_state *state, FILE *f) {
	char hex[10]; color_to_hex(state->config.colors.button_hover, hex);
	fprintf(f, "color_button_hover=%s\n", hex);
}

static config_entry_t config_entries[] = {
	{"auto_save_enabled", "Enable auto-saving screenshots after copy", parse_auto_save_enabled, format_auto_save_enabled, false},
	{"auto_save_path", "Path to save screenshots", parse_auto_save_path, format_auto_save_path, false},
	{"auto_save_format", "Format to save (png, jpg)", parse_auto_save_format, format_auto_save_format, false},
	{"auto_save_filename_format", "Filename format (strftime syntax)", parse_auto_save_filename_format, format_auto_save_filename_format, false},
	{"color_accent", "Accent color (#RRGGBBAA)", parse_color_accent, format_color_accent, false},
	{"color_toolbar_bg", "Toolbar background color", parse_color_toolbar_bg, format_color_toolbar_bg, false},
	{"color_button_hover", "Button hover color", parse_color_button_hover, format_color_button_hover, false},
};
#define CONFIG_ENTRY_COUNT (sizeof(config_entries)/sizeof(config_entries[0]))

void config_init(struct escreen_state *state) {
	// Defaults
	state->config.auto_save_enabled = false;
	const char *home = getenv("HOME");
	char buf[512];
	if (home) {
		snprintf(buf, sizeof(buf), "%s/Pictures/escreen", home);
		state->config.auto_save_path = strdup(buf);
	} else {
		state->config.auto_save_path = strdup("escreen-captures");
	}
	state->config.auto_save_filename_format = strdup("escreen_%Y%m%d_%H%M%S");

	state->config.colors.accent = (escreen_color_t){0.0, 0.62, 1.0, 1.0};
	state->config.colors.toolbar_bg = (escreen_color_t){0.15, 0.15, 0.15, 0.95};
	state->config.colors.button_hover = (escreen_color_t){0.3, 0.3, 0.3, 1.0};
}

void config_load(struct escreen_state *state) {
	const char *home = getenv("HOME");
	if (!home) return;

	char cfg_dir[512];
	snprintf(cfg_dir, sizeof(cfg_dir), "%s/.config/escreen", home);
	mkdir(cfg_dir, 0755);

	char path[512];
	snprintf(path, sizeof(path), "%s/config", cfg_dir);

	FILE *f = fopen(path, "r");
	bool needs_repair = !f;

	if (f) {
		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			char *p = line;
			while (isspace(*p)) p++;
			if (*p == '#' || *p == '\0') continue;

			char *key = p;
			char *eq = strchr(p, '=');
			if (!eq) continue;
			*eq = '\0';
			char *val = eq + 1;

			// Trim whitespace
			char *end = key + strlen(key) - 1;
			while (end >= key && isspace(*end)) { *end = '\0'; end--; }
			while (isspace(*val)) val++;
			end = val + strlen(val) - 1;
			while (end >= val && (isspace(*end) || *end == '\n' || *end == '\r')) { *end = '\0'; end--; }

			for (size_t i = 0; i < CONFIG_ENTRY_COUNT; i++) {
				if (strcmp(key, config_entries[i].key) == 0) {
					config_entries[i].parse(state, val);
					config_entries[i].seen = true;
					break;
				}
			}
		}
		fclose(f);

		for (size_t i = 0; i < CONFIG_ENTRY_COUNT; i++) {
			if (!config_entries[i].seen) {
				needs_repair = true;
				break;
			}
		}
	}

	if (needs_repair) {
		if (f) {
			fprintf(stderr, "Escreen [WARNING]: Configuration file missing fields. Repairing...\n");
			for (size_t i = 0; i < CONFIG_ENTRY_COUNT; i++) {
				if (!config_entries[i].seen) {
					fprintf(stderr, "  - Restoring missing key: %s\n", config_entries[i].key);
				}
			}
		} else {
			fprintf(stderr, "Escreen: Initializing default configuration at %s\n", path);
		}

		f = fopen(path, "w");
		if (f) {
			fprintf(f, "# Escreen Configuration File\n\n");
			for (size_t i = 0; i < CONFIG_ENTRY_COUNT; i++) {
				fprintf(f, "# %s\n", config_entries[i].description);
				config_entries[i].format(state, f);
				fprintf(f, "\n");
			}
			fclose(f);
		}
	}
}
