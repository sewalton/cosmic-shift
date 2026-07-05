/*
 * cosmic-shift - a redshift-style color temperature daemon for Wayland,
 * built for Pop!_OS / COSMIC.
 *
 * Two backends, picked automatically:
 *
 *   gamma   - wlr-gamma-control-unstable-v1 hardware gamma tables, exactly
 *             like gammastep/wlsunset. Used when the compositor offers it
 *             (sway, Hyprland, river, and COSMIC once System76 ships gamma
 *             support).
 *
 *   overlay - a full-screen, click-through, warm-tinted layer-shell surface
 *             on every output. COSMIC does not implement any gamma protocol
 *             yet, so this is what makes cosmic-shift work on Pop!_OS today.
 *             The overlay color is chosen so that white maps to exactly the
 *             same blackbody white point the gamma backend would produce.
 *
 * Wayland state dies with the client, so cosmic-shift keeps running until
 * interrupted; Ctrl+C (or killing the process) restores normal colors.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"

#define TEMP_MIN 1000
#define TEMP_MAX 10000
#define TEMP_NEUTRAL 6500

/* Solar elevation thresholds, matching redshift's defaults: full day color
 * above 3 degrees, full night color below -6 degrees (civil dusk). */
#define ELEV_DAY 3.0
#define ELEV_NIGHT -6.0

enum backend {
	BACKEND_AUTO,
	BACKEND_GAMMA,
	BACKEND_OVERLAY,
};

struct config {
	int temp;          /* constant mode temperature */
	int temp_day;      /* auto mode day temperature */
	int temp_night;    /* auto mode night temperature */
	double brightness; /* 0.1 .. 1.0 */
	double tint;       /* overlay tint strength, 0.0 .. 1.0 */
	double gamma;      /* gamma exponent, 0.5 .. 2.0 (gamma backend only) */
	bool auto_mode;    /* follow the sun */
	double lat, lon;
	enum backend backend;
	bool control;      /* -i: read live commands from stdin, exit on EOF */
	bool verbose;
};

struct output {
	struct wl_list link;
	struct wl_output *wl_output;
	uint32_t registry_name;
	bool pending; /* needs the current color (re)applied */

	/* gamma backend */
	struct zwlr_gamma_control_v1 *gamma_control;
	uint32_t ramp_size;

	/* overlay backend */
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wp_viewport *viewport;
	uint32_t width, height;
	bool overlay_configured;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct zwlr_gamma_control_manager_v1 *gamma_manager;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wp_viewporter *viewporter;
static struct wp_single_pixel_buffer_manager_v1 *single_pixel_mgr;
static struct wl_list outputs; /* struct output */
static enum backend backend;
static struct wl_buffer *overlay_buffer;     /* currently attached */
static struct wl_buffer *overlay_buffer_old; /* kept one cycle, then freed */
static volatile sig_atomic_t running = 1;

static struct config cfg = {
	.temp = 4500,
	.temp_day = 6500,
	.temp_night = 4000,
	.brightness = 1.0,
	.tint = 0.5,
	.gamma = 1.0,
	.auto_mode = false,
	.backend = BACKEND_AUTO,
	.control = false,
	.verbose = false,
};

/* --- stdin control (-i) ------------------------------------------------ */

/*
 * Line protocol used by cosmic-shift-gtk (and anything else that wants to
 * drive a running instance): "temp 3000", "tint 0.05", "brightness 0.9",
 * "gamma 1.1". EOF on stdin means the controller went away; exit and
 * restore normal colors.
 */
static char control_buf[256];
static size_t control_len;
static bool control_dirty;

static int clamp_temp(long t);

static void handle_control_line(const char *line) {
	long t;
	double v;
	if (sscanf(line, "temp %ld", &t) == 1) {
		cfg.temp = clamp_temp(t);
		cfg.auto_mode = false;
		control_dirty = true;
	} else if (sscanf(line, "tint %lf", &v) == 1) {
		cfg.tint = fmin(fmax(v, 0.0), 1.0);
		control_dirty = true;
	} else if (sscanf(line, "brightness %lf", &v) == 1) {
		cfg.brightness = fmin(fmax(v, 0.1), 1.0);
		control_dirty = true;
	} else if (sscanf(line, "gamma %lf", &v) == 1) {
		cfg.gamma = fmin(fmax(v, 0.5), 2.0);
		control_dirty = true;
	} else if (line[0] != '\0' && cfg.verbose) {
		fprintf(stderr, "cosmic-shift: ignoring control line '%s'\n", line);
	}
}

/* Returns false when stdin hit EOF (controller gone). */
static bool read_control_input(void) {
	for (;;) {
		ssize_t n = read(STDIN_FILENO, control_buf + control_len,
		                 sizeof(control_buf) - control_len - 1);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return true;
			if (errno == EINTR)
				continue;
			return false;
		}
		if (n == 0)
			return false;
		control_len += n;
		control_buf[control_len] = '\0';

		char *start = control_buf, *nl;
		while ((nl = strchr(start, '\n')) != NULL) {
			*nl = '\0';
			handle_control_line(start);
			start = nl + 1;
		}
		control_len = strlen(start);
		memmove(control_buf, start, control_len + 1);
		if (control_len == sizeof(control_buf) - 1)
			control_len = 0; /* garbage line longer than the buffer */
	}
}

static void handle_signal(int sig) {
	(void)sig;
	running = 0;
}

/*
 * Blackbody whitepoint approximation (Tanner Helland / redshift style).
 * Maps a color temperature in Kelvin to linear RGB multipliers in [0, 1].
 */
static void calc_whitepoint(int temp, double *rw, double *gw, double *bw) {
	double t = temp / 100.0;
	double r, g, b;

	if (t <= 66.0) {
		r = 255.0;
	} else {
		r = 329.698727446 * pow(t - 60.0, -0.1332047592);
	}

	if (t <= 66.0) {
		g = 99.4708025861 * log(t) - 161.1195681661;
	} else {
		g = 288.1221695283 * pow(t - 60.0, -0.0755148492);
	}

	if (t >= 66.0) {
		b = 255.0;
	} else if (t <= 19.0) {
		b = 0.0;
	} else {
		b = 138.5177312231 * log(t - 10.0) - 305.0447927307;
	}

	*rw = fmin(fmax(r / 255.0, 0.0), 1.0);
	*gw = fmin(fmax(g / 255.0, 0.0), 1.0);
	*bw = fmin(fmax(b / 255.0, 0.0), 1.0);
}

/*
 * Approximate solar elevation in degrees for a given time and location.
 * Standard low-precision astronomical algorithm (accurate to well under a
 * degree, which is plenty for choosing screen color).
 */
static double solar_elevation(time_t when, double lat, double lon) {
	double d = when / 86400.0 + 2440587.5 - 2451545.0; /* days since J2000 */
	double rad = M_PI / 180.0;

	double g = fmod(357.529 + 0.98560028 * d, 360.0); /* mean anomaly */
	double q = fmod(280.459 + 0.98564736 * d, 360.0); /* mean longitude */
	double L = q + 1.915 * sin(g * rad) + 0.020 * sin(2.0 * g * rad);
	double e = 23.439 - 0.00000036 * d; /* obliquity of the ecliptic */

	double ra = atan2(cos(e * rad) * sin(L * rad), cos(L * rad)) / rad / 15.0;
	if (ra < 0.0)
		ra += 24.0;
	double decl = asin(sin(e * rad) * sin(L * rad)) / rad;

	double gmst = fmod(18.697374558 + 24.06570982441908 * d, 24.0);
	double ha = (fmod(gmst + lon / 15.0 - ra, 24.0)) * 15.0 * rad;

	double elev = asin(sin(lat * rad) * sin(decl * rad) +
	                   cos(lat * rad) * cos(decl * rad) * cos(ha));
	return elev / rad;
}

/* Interpolate between night and day temperature based on solar elevation. */
static int temp_for_elevation(double elev) {
	if (elev >= ELEV_DAY)
		return cfg.temp_day;
	if (elev <= ELEV_NIGHT)
		return cfg.temp_night;
	double f = (elev - ELEV_NIGHT) / (ELEV_DAY - ELEV_NIGHT);
	return (int)(cfg.temp_night + f * (cfg.temp_day - cfg.temp_night));
}

/* --- gamma backend ---------------------------------------------------- */

static int create_gamma_table_fd(size_t bytes) {
	int fd = memfd_create("cosmic-shift-gamma", MFD_CLOEXEC);
	if (fd < 0) {
		perror("memfd_create");
		return -1;
	}
	if (ftruncate(fd, bytes) < 0) {
		perror("ftruncate");
		close(fd);
		return -1;
	}
	return fd;
}

static void fill_gamma_table(uint16_t *table, uint32_t size, int temp) {
	double rw, gw, bw;
	calc_whitepoint(temp, &rw, &gw, &bw);

	uint16_t *r = table;
	uint16_t *g = table + size;
	uint16_t *b = table + 2 * size;

	for (uint32_t i = 0; i < size; i++) {
		double val = (double)i / (size > 1 ? size - 1 : 1);
		if (cfg.gamma != 1.0)
			val = pow(val, 1.0 / cfg.gamma);
		val *= cfg.brightness;
		r[i] = (uint16_t)(UINT16_MAX * val * rw);
		g[i] = (uint16_t)(UINT16_MAX * val * gw);
		b[i] = (uint16_t)(UINT16_MAX * val * bw);
	}
}

static bool set_output_gamma(struct output *out, int temp) {
	if (out->gamma_control == NULL || out->ramp_size == 0)
		return false;

	size_t bytes = out->ramp_size * 3 * sizeof(uint16_t);
	int fd = create_gamma_table_fd(bytes);
	if (fd < 0)
		return false;

	uint16_t *table = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
	                       MAP_SHARED, fd, 0);
	if (table == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return false;
	}

	fill_gamma_table(table, out->ramp_size, temp);
	munmap(table, bytes);

	zwlr_gamma_control_v1_set_gamma(out->gamma_control, fd);
	close(fd);
	out->pending = false;
	return true;
}

static void gamma_control_handle_gamma_size(void *data,
		struct zwlr_gamma_control_v1 *control, uint32_t size) {
	(void)control;
	struct output *out = data;
	out->ramp_size = size;
	out->pending = true;
}

static void gamma_control_handle_failed(void *data,
		struct zwlr_gamma_control_v1 *control) {
	(void)control;
	struct output *out = data;
	fprintf(stderr, "cosmic-shift: gamma control failed for an output "
	        "(is another program controlling gamma?)\n");
	zwlr_gamma_control_v1_destroy(out->gamma_control);
	out->gamma_control = NULL;
	out->ramp_size = 0;
	out->pending = false;
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
	.gamma_size = gamma_control_handle_gamma_size,
	.failed = gamma_control_handle_failed,
};

static void output_setup_gamma_control(struct output *out) {
	if (gamma_manager == NULL || out->gamma_control != NULL)
		return;
	out->gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(
		gamma_manager, out->wl_output);
	zwlr_gamma_control_v1_add_listener(out->gamma_control,
		&gamma_control_listener, out);
}

/* --- overlay backend --------------------------------------------------- */

/*
 * Choose a translucent overlay color approximating what hardware gamma
 * would do: out = brightness * whitepoint * pixel. Alpha blending can only
 * produce  out = src + (1 - alpha) * dst,  an affine function with the
 * same slope for every channel, so a perfect per-channel multiply is
 * impossible: blue can only be reduced *relative* to red and green by
 * darkening every channel and painting some red/green back on top.
 *
 * The tint knob s (default 0.5) sets how much gets painted back:
 * alpha = 1 - b*min(w) and premultiplied rgb = s * b * (w - min(w)).
 * s = 1 keeps whites at the exact blackbody color but floods dark content
 * with the tint; s = 0 adds no color at all and turns the overlay into
 * pure dimming; s = 0.5 is the least-squares fit to true gamma, exact for
 * mid-tones. At 6500K with brightness 1.0 the overlay is fully
 * transparent.
 */
static struct wl_buffer *create_overlay_buffer(int temp) {
	double rw, gw, bw;
	calc_whitepoint(temp, &rw, &gw, &bw);

	double b = cfg.brightness;
	double minw = fmin(rw, fmin(gw, bw));
	double alpha = 1.0 - b * minw;
	double r = cfg.tint * b * (rw - minw);
	double g = cfg.tint * b * (gw - minw);
	double bl = cfg.tint * b * (bw - minw);

	return wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
		single_pixel_mgr,
		(uint32_t)(r * (double)UINT32_MAX),
		(uint32_t)(g * (double)UINT32_MAX),
		(uint32_t)(bl * (double)UINT32_MAX),
		(uint32_t)(alpha * (double)UINT32_MAX));
}

static void destroy_output_overlay(struct output *out) {
	if (out->layer_surface != NULL)
		zwlr_layer_surface_v1_destroy(out->layer_surface);
	if (out->viewport != NULL)
		wp_viewport_destroy(out->viewport);
	if (out->surface != NULL)
		wl_surface_destroy(out->surface);
	out->layer_surface = NULL;
	out->viewport = NULL;
	out->surface = NULL;
	out->overlay_configured = false;
	out->pending = false;
}

static void layer_surface_handle_configure(void *data,
		struct zwlr_layer_surface_v1 *ls, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct output *out = data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	out->width = width;
	out->height = height;
	out->overlay_configured = true;
	out->pending = true;
}

static void layer_surface_handle_closed(void *data,
		struct zwlr_layer_surface_v1 *ls) {
	(void)ls;
	destroy_output_overlay(data);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_handle_configure,
	.closed = layer_surface_handle_closed,
};

static void output_setup_overlay(struct output *out) {
	if (out->surface != NULL)
		return;

	out->surface = wl_compositor_create_surface(compositor);

	/* Empty input region: clicks and touches pass straight through. */
	struct wl_region *empty = wl_compositor_create_region(compositor);
	wl_surface_set_input_region(out->surface, empty);
	wl_region_destroy(empty);

	out->viewport = wp_viewporter_get_viewport(viewporter, out->surface);

	out->layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
		out->surface, out->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		"cosmic-shift");
	zwlr_layer_surface_v1_add_listener(out->layer_surface,
		&layer_surface_listener, out);
	zwlr_layer_surface_v1_set_anchor(out->layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(out->layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(out->layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

	/* Initial commit without a buffer requests a configure event. */
	wl_surface_commit(out->surface);
}

static bool set_output_overlay(struct output *out, struct wl_buffer *buffer) {
	if (!out->overlay_configured || out->width == 0 || out->height == 0)
		return false;

	wp_viewport_set_destination(out->viewport, out->width, out->height);
	wl_surface_attach(out->surface, buffer, 0, 0);
	wl_surface_damage_buffer(out->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(out->surface);
	out->pending = false;
	return true;
}

/* --- apply -------------------------------------------------------------- */

static void apply_temperature(int temp) {
	struct output *out;
	int applied = 0;

	if (backend == BACKEND_GAMMA) {
		wl_list_for_each(out, &outputs, link) {
			if (set_output_gamma(out, temp))
				applied++;
		}
	} else {
		struct wl_buffer *buffer = create_overlay_buffer(temp);
		wl_list_for_each(out, &outputs, link) {
			if (set_output_overlay(out, buffer))
				applied++;
		}
		/* The previous buffer may still be current on some surface until
		 * the compositor processes the commits, so free it one cycle
		 * late. */
		if (overlay_buffer_old != NULL)
			wl_buffer_destroy(overlay_buffer_old);
		overlay_buffer_old = overlay_buffer;
		overlay_buffer = buffer;
	}

	if (cfg.verbose)
		fprintf(stderr, "cosmic-shift: %dK, brightness %.2f on %d output(s)\n",
		        temp, cfg.brightness, applied);
}

/* --- Wayland registry --------------------------------------------------- */

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data;
	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *out = calloc(1, sizeof(*out));
		out->registry_name = name;
		out->wl_output = wl_registry_bind(registry, name,
			&wl_output_interface, 1);
		wl_list_insert(&outputs, &out->link);
		/* Backend not chosen yet during the first roundtrip; main() sets
		 * up initial outputs. Hotplugged outputs are handled here. */
		if (backend == BACKEND_GAMMA)
			output_setup_gamma_control(out);
		else if (backend == BACKEND_OVERLAY)
			output_setup_overlay(out);
	} else if (strcmp(interface,
			zwlr_gamma_control_manager_v1_interface.name) == 0) {
		gamma_manager = wl_registry_bind(registry, name,
			&zwlr_gamma_control_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(registry, name,
			&wp_viewporter_interface, 1);
	} else if (strcmp(interface,
			wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
		single_pixel_mgr = wl_registry_bind(registry, name,
			&wp_single_pixel_buffer_manager_v1_interface, 1);
	}
}

static void registry_handle_global_remove(void *data,
		struct wl_registry *registry, uint32_t name) {
	(void)data;
	(void)registry;
	struct output *out, *tmp;
	wl_list_for_each_safe(out, tmp, &outputs, link) {
		if (out->registry_name == name) {
			if (out->gamma_control != NULL)
				zwlr_gamma_control_v1_destroy(out->gamma_control);
			destroy_output_overlay(out);
			wl_output_destroy(out->wl_output);
			wl_list_remove(&out->link);
			free(out);
			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

/* --- CLI ------------------------------------------------------------- */

static void usage(const char *prog) {
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"A redshift replacement for Wayland, built for Pop!_OS / COSMIC.\n"
		"Runs until interrupted; Ctrl+C restores normal colors.\n"
		"\n"
		"Options:\n"
		"  -t TEMP       constant color temperature in Kelvin (default 4500)\n"
		"  -l LAT:LON    location; enables automatic day/night mode\n"
		"  -T DAY:NIGHT  temperatures for automatic mode (default 6500:4000)\n"
		"  -b BRIGHT     brightness multiplier, 0.1-1.0 (default 1.0)\n"
		"  -s TINT       overlay tint strength, 0.0-1.0 (default 0.5);\n"
		"                lower = less yellow cast, more dimming\n"
		"  -g GAMMA      gamma exponent, 0.5-2.0 (gamma backend only)\n"
		"  -m MODE       backend: auto, gamma, overlay (default auto)\n"
		"  -i            control mode: read live setting changes from stdin\n"
		"                (temp N / tint X / brightness X / gamma X), exit on EOF\n"
		"  -v            verbose output\n"
		"  -h            show this help\n"
		"\n"
		"Examples:\n"
		"  %s -t 3500              warm evening screen\n"
		"  %s -l 40.7:-74.0        follow the sun in New York\n",
		prog, prog, prog);
}

static int clamp_temp(long t) {
	if (t < TEMP_MIN) return TEMP_MIN;
	if (t > TEMP_MAX) return TEMP_MAX;
	return (int)t;
}

static bool parse_args(int argc, char *argv[]) {
	int opt;
	while ((opt = getopt(argc, argv, "t:l:T:b:s:g:m:ivh")) != -1) {
		switch (opt) {
		case 't':
			cfg.temp = clamp_temp(strtol(optarg, NULL, 10));
			break;
		case 'l': {
			if (sscanf(optarg, "%lf:%lf", &cfg.lat, &cfg.lon) != 2 ||
			    cfg.lat < -90.0 || cfg.lat > 90.0 ||
			    cfg.lon < -180.0 || cfg.lon > 180.0) {
				fprintf(stderr, "invalid location '%s', expected LAT:LON\n",
				        optarg);
				return false;
			}
			cfg.auto_mode = true;
			break;
		}
		case 'T': {
			long d, n;
			if (sscanf(optarg, "%ld:%ld", &d, &n) != 2) {
				fprintf(stderr, "invalid temperatures '%s', "
				        "expected DAY:NIGHT\n", optarg);
				return false;
			}
			cfg.temp_day = clamp_temp(d);
			cfg.temp_night = clamp_temp(n);
			break;
		}
		case 'b':
			cfg.brightness = strtod(optarg, NULL);
			if (cfg.brightness < 0.1) cfg.brightness = 0.1;
			if (cfg.brightness > 1.0) cfg.brightness = 1.0;
			break;
		case 's':
			cfg.tint = strtod(optarg, NULL);
			if (cfg.tint < 0.0) cfg.tint = 0.0;
			if (cfg.tint > 1.0) cfg.tint = 1.0;
			break;
		case 'g':
			cfg.gamma = strtod(optarg, NULL);
			if (cfg.gamma < 0.5) cfg.gamma = 0.5;
			if (cfg.gamma > 2.0) cfg.gamma = 2.0;
			break;
		case 'm':
			if (strcmp(optarg, "auto") == 0)
				cfg.backend = BACKEND_AUTO;
			else if (strcmp(optarg, "gamma") == 0)
				cfg.backend = BACKEND_GAMMA;
			else if (strcmp(optarg, "overlay") == 0)
				cfg.backend = BACKEND_OVERLAY;
			else {
				fprintf(stderr, "invalid mode '%s', expected "
				        "auto, gamma or overlay\n", optarg);
				return false;
			}
			break;
		case 'i':
			cfg.control = true;
			break;
		case 'v':
			cfg.verbose = true;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			return false;
		}
	}
	return true;
}

static int current_target_temp(void) {
	if (!cfg.auto_mode)
		return cfg.temp;
	double elev = solar_elevation(time(NULL), cfg.lat, cfg.lon);
	int temp = temp_for_elevation(elev);
	if (cfg.verbose)
		fprintf(stderr, "cosmic-shift: solar elevation %.1f°, target %dK\n",
		        elev, temp);
	return temp;
}

static bool overlay_available(void) {
	return compositor != NULL && layer_shell != NULL &&
	       viewporter != NULL && single_pixel_mgr != NULL;
}

int main(int argc, char *argv[]) {
	if (!parse_args(argc, argv))
		return 1;

	struct sigaction sa = { .sa_handler = handle_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	wl_list_init(&outputs);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "cosmic-shift: cannot connect to Wayland display "
		        "(is this a Wayland session?)\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	switch (cfg.backend) {
	case BACKEND_GAMMA:
		if (gamma_manager == NULL) {
			fprintf(stderr, "cosmic-shift: compositor does not support "
			        "wlr-gamma-control-unstable-v1\n");
			return 1;
		}
		backend = BACKEND_GAMMA;
		break;
	case BACKEND_OVERLAY:
		if (!overlay_available()) {
			fprintf(stderr, "cosmic-shift: compositor lacks the protocols "
			        "needed for the overlay backend\n");
			return 1;
		}
		backend = BACKEND_OVERLAY;
		break;
	case BACKEND_AUTO:
		if (gamma_manager != NULL) {
			backend = BACKEND_GAMMA;
		} else if (overlay_available()) {
			backend = BACKEND_OVERLAY;
			fprintf(stderr, "cosmic-shift: compositor has no gamma "
			        "protocol (COSMIC doesn't yet); using the overlay "
			        "backend\n");
		} else {
			fprintf(stderr, "cosmic-shift: compositor supports neither "
			        "gamma control nor layer-shell overlays\n");
			return 1;
		}
		break;
	}

	/* Set up outputs discovered during the first roundtrip; a second
	 * roundtrip delivers gamma_size / configure events. */
	struct output *out;
	wl_list_for_each(out, &outputs, link) {
		if (backend == BACKEND_GAMMA)
			output_setup_gamma_control(out);
		else
			output_setup_overlay(out);
	}
	wl_display_roundtrip(display);

	int temp = current_target_temp();
	apply_temperature(temp);

	if (cfg.control)
		fcntl(STDIN_FILENO, F_SETFL,
		      fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
	bool control_open = cfg.control;

	while (running) {
		if (wl_display_flush(display) < 0 && errno != EAGAIN)
			break;

		/* In auto mode re-evaluate the sun every 60 s; otherwise just
		 * sit on the fds to service hotplug, configure and control
		 * events. */
		int timeout_ms = cfg.auto_mode ? 60 * 1000 : -1;

		struct pollfd pfds[2] = {
			{ .fd = wl_display_get_fd(display), .events = POLLIN },
			{ .fd = STDIN_FILENO, .events = POLLIN },
		};
		int ret = poll(pfds, control_open ? 2 : 1, timeout_ms);
		if (ret < 0) {
			if (errno == EINTR)
				continue; /* signal: loop re-checks 'running' */
			perror("poll");
			break;
		}

		if (pfds[0].revents & POLLIN) {
			if (wl_display_dispatch(display) < 0) {
				fprintf(stderr, "cosmic-shift: lost connection "
				        "to compositor\n");
				break;
			}
		}

		if (control_open &&
		    (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
			if (!read_control_input()) {
				/* Controller disconnected: restore and exit. */
				running = 0;
				break;
			}
		}

		int target = current_target_temp();
		bool need_apply = target != temp || control_dirty;
		control_dirty = false;

		/* Newly hotplugged or resized outputs need the current color. */
		wl_list_for_each(out, &outputs, link) {
			if (out->pending)
				need_apply = true;
		}

		if (need_apply) {
			temp = target;
			apply_temperature(temp);
		}
	}

	if (cfg.verbose)
		fprintf(stderr, "cosmic-shift: exiting, restoring gamma\n");

	/* Tearing everything down makes the compositor restore original
	 * colors: gamma tables revert, overlay surfaces disappear. */
	struct output *tmp;
	wl_list_for_each_safe(out, tmp, &outputs, link) {
		if (out->gamma_control != NULL)
			zwlr_gamma_control_v1_destroy(out->gamma_control);
		destroy_output_overlay(out);
		wl_output_destroy(out->wl_output);
		wl_list_remove(&out->link);
		free(out);
	}
	if (overlay_buffer != NULL)
		wl_buffer_destroy(overlay_buffer);
	if (overlay_buffer_old != NULL)
		wl_buffer_destroy(overlay_buffer_old);
	if (gamma_manager != NULL)
		zwlr_gamma_control_manager_v1_destroy(gamma_manager);
	if (layer_shell != NULL)
		zwlr_layer_shell_v1_destroy(layer_shell);
	if (viewporter != NULL)
		wp_viewporter_destroy(viewporter);
	if (single_pixel_mgr != NULL)
		wp_single_pixel_buffer_manager_v1_destroy(single_pixel_mgr);
	if (compositor != NULL)
		wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_flush(display);
	wl_display_disconnect(display);
	return 0;
}
