#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define E8_DIM 8
#define E8_ROOT_COUNT 240
#define COXETER_NUMBER 30
#define PI 3.14159265358979323846
#define MAX_CELLS 2000000U

static const char DENSITY_RAMP[] = " .,:;irsXA253hMHGS#9B&@";
static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t resize_pending = 1;
static int terminal_active = 0;

typedef struct {
    double value[E8_DIM];
} Vec8;

typedef struct {
    float energy;
    float radius_sum;
} Cell;

typedef struct {
    Cell *cells;
    int width;
    int height;
    char *output;
    size_t output_capacity;
} Renderer;

typedef struct {
    int fps;
    double cycle_seconds;
    unsigned long frame_limit;
    int color;
    int rays;
} Options;

typedef struct {
    int first_axis;
    int second_axis;
    double amplitude;
    double harmonic;
    double offset;
} PlaneMotion;

static const PlaneMotion MOTION[] = {
    {0, 1, 0.42, 1.0, 0.10},
    {2, 3, 0.51, 2.0, 1.20},
    {4, 5, 0.47, 3.0, 2.10},
    {6, 7, 0.56, 2.0, 2.80},
    {0, 7, 0.68, 1.0, 1.70},
    {1, 6, 0.59, 3.0, 0.80},
    {2, 5, 0.64, 2.0, 2.50},
    {3, 4, 0.53, 4.0, 0.30},
    {0, 4, 0.36, 5.0, 1.00},
    {1, 5, 0.40, 4.0, 2.20},
    {2, 6, 0.38, 5.0, 2.90},
    {3, 7, 0.44, 3.0, 1.50},
};

static const Vec8 SIMPLE_ROOTS[E8_DIM] = {
    {{ 0.5, -0.5, -0.5, -0.5, -0.5, -0.5, -0.5,  0.5}},
    {{ 1.0,  1.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0}},
    {{-1.0,  1.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0}},
    {{ 0.0, -1.0,  1.0,  0.0,  0.0,  0.0,  0.0,  0.0}},
    {{ 0.0,  0.0, -1.0,  1.0,  0.0,  0.0,  0.0,  0.0}},
    {{ 0.0,  0.0,  0.0, -1.0,  1.0,  0.0,  0.0,  0.0}},
    {{ 0.0,  0.0,  0.0,  0.0, -1.0,  1.0,  0.0,  0.0}},
    {{ 0.0,  0.0,  0.0,  0.0,  0.0, -1.0,  1.0,  0.0}},
};

static void handle_stop(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

static void handle_resize(int signal_number) {
    (void)signal_number;
    resize_pending = 1;
}

static void restore_terminal(void) {
    if (terminal_active) {
        fputs("\033[0m\033[?25h\033[?1049l", stdout);
        fflush(stdout);
        terminal_active = 0;
    }
}

static double dot8(const Vec8 *left, const Vec8 *right) {
    double result = 0.0;
    for (int axis = 0; axis < E8_DIM; ++axis) {
        result += left->value[axis] * right->value[axis];
    }
    return result;
}

static int popcount8(unsigned int value) {
    int count = 0;
    for (; value != 0; value >>= 1U) {
        count += (int)(value & 1U);
    }
    return count;
}

static size_t generate_e8_roots(Vec8 roots[E8_ROOT_COUNT]) {
    size_t count = 0;

    for (int first = 0; first < E8_DIM; ++first) {
        for (int second = first + 1; second < E8_DIM; ++second) {
            for (unsigned int signs = 0; signs < 4; ++signs) {
                Vec8 root = {{0.0}};
                root.value[first] = (signs & 1U) ? -1.0 : 1.0;
                root.value[second] = (signs & 2U) ? -1.0 : 1.0;
                roots[count++] = root;
            }
        }
    }

    for (unsigned int signs = 0; signs < 256; ++signs) {
        if ((popcount8(signs) & 1) != 0) {
            continue;
        }
        Vec8 root;
        for (int axis = 0; axis < E8_DIM; ++axis) {
            root.value[axis] = (signs & (1U << axis)) ? -0.5 : 0.5;
        }
        roots[count++] = root;
    }

    return count;
}

static void reflect(Vec8 *vector, const Vec8 *root) {
    /* E8 roots have squared length 2, hence x' = x - (x.a)a. */
    double projection = dot8(vector, root);
    for (int axis = 0; axis < E8_DIM; ++axis) {
        vector->value[axis] -= projection * root->value[axis];
    }
}

static void apply_coxeter(Vec8 *vector) {
    for (int root = 0; root < E8_DIM; ++root) {
        reflect(vector, &SIMPLE_ROOTS[root]);
    }
}

static int normalize(Vec8 *vector) {
    double length = sqrt(dot8(vector, vector));
    if (length < 1e-12) {
        return 0;
    }
    for (int axis = 0; axis < E8_DIM; ++axis) {
        vector->value[axis] /= length;
    }
    return 1;
}

static int build_coxeter_plane(Vec8 *horizontal, Vec8 *vertical) {
    Vec8 orbit = SIMPLE_ROOTS[0];
    memset(horizontal, 0, sizeof(*horizontal));
    memset(vertical, 0, sizeof(*vertical));

    /* Fourier extraction of the exponent-1 eigenspace of the Coxeter map. */
    for (int step = 0; step < COXETER_NUMBER; ++step) {
        double angle = 2.0 * PI * step / COXETER_NUMBER;
        for (int axis = 0; axis < E8_DIM; ++axis) {
            horizontal->value[axis] += cos(angle) * orbit.value[axis];
            vertical->value[axis] += sin(angle) * orbit.value[axis];
        }
        apply_coxeter(&orbit);
    }

    if (!normalize(horizontal)) {
        return 0;
    }
    double overlap = dot8(horizontal, vertical);
    for (int axis = 0; axis < E8_DIM; ++axis) {
        vertical->value[axis] -= overlap * horizontal->value[axis];
    }
    return normalize(vertical);
}

static int validate_geometry(
    const Vec8 roots[E8_ROOT_COUNT],
    size_t count,
    const Vec8 *horizontal,
    const Vec8 *vertical
) {
    if (count != E8_ROOT_COUNT) {
        fprintf(stderr, "E8 validation failed: generated %zu roots, expected %d\n",
                count, E8_ROOT_COUNT);
        return 0;
    }

    for (size_t index = 0; index < count; ++index) {
        if (fabs(dot8(&roots[index], &roots[index]) - 2.0) > 1e-10) {
            fprintf(stderr, "E8 validation failed: root %zu has invalid norm\n", index);
            return 0;
        }
        for (size_t other = index + 1; other < count; ++other) {
            double distance = 0.0;
            for (int axis = 0; axis < E8_DIM; ++axis) {
                distance += fabs(roots[index].value[axis] - roots[other].value[axis]);
            }
            if (distance < 1e-10) {
                fprintf(stderr, "E8 validation failed: duplicate roots %zu and %zu\n",
                        index, other);
                return 0;
            }
        }
    }

    if (fabs(dot8(horizontal, horizontal) - 1.0) > 1e-10
        || fabs(dot8(vertical, vertical) - 1.0) > 1e-10
        || fabs(dot8(horizontal, vertical)) > 1e-10) {
        fputs("E8 validation failed: Coxeter plane is not orthonormal\n", stderr);
        return 0;
    }

    Vec8 orbit = SIMPLE_ROOTS[0];
    for (int step = 0; step < COXETER_NUMBER; ++step) {
        apply_coxeter(&orbit);
    }
    for (int axis = 0; axis < E8_DIM; ++axis) {
        if (fabs(orbit.value[axis] - SIMPLE_ROOTS[0].value[axis]) > 1e-10) {
            fputs("E8 validation failed: Coxeter map does not have order 30\n", stderr);
            return 0;
        }
    }

    int visited[E8_ROOT_COUNT] = {0};
    int orbit_count = 0;
    for (size_t start = 0; start < count; ++start) {
        if (visited[start]) {
            continue;
        }
        ++orbit_count;
        orbit = roots[start];
        for (int step = 0; step < COXETER_NUMBER; ++step) {
            size_t match = count;
            for (size_t candidate = 0; candidate < count; ++candidate) {
                double distance = 0.0;
                for (int axis = 0; axis < E8_DIM; ++axis) {
                    distance += fabs(orbit.value[axis] - roots[candidate].value[axis]);
                }
                if (distance < 1e-10) {
                    match = candidate;
                    break;
                }
            }
            if (match == count || visited[match]) {
                fputs("E8 validation failed: invalid Coxeter orbit\n", stderr);
                return 0;
            }
            visited[match] = 1;
            apply_coxeter(&orbit);
        }
    }
    if (orbit_count != E8_ROOT_COUNT / COXETER_NUMBER) {
        fprintf(stderr, "E8 validation failed: found %d Coxeter orbits, expected 8\n",
                orbit_count);
        return 0;
    }

    return 1;
}

static void rotate_plane(Vec8 *vector, int first, int second, double angle) {
    double cosine = cos(angle);
    double sine = sin(angle);
    double a = vector->value[first];
    double b = vector->value[second];
    vector->value[first] = a * cosine - b * sine;
    vector->value[second] = a * sine + b * cosine;
}

static void animate_root(Vec8 *root, double phase) {
    double deformation = 0.5 - 0.5 * cos(phase);
    size_t motion_count = sizeof(MOTION) / sizeof(MOTION[0]);

    for (size_t index = 0; index < motion_count; ++index) {
        const PlaneMotion *motion = &MOTION[index];
        double angle = deformation * motion->amplitude
            * sin(motion->harmonic * phase + motion->offset);
        rotate_plane(root, motion->first_axis, motion->second_axis, angle);
    }
}

static double projected_radius_limit(
    const Vec8 roots[E8_ROOT_COUNT],
    const Vec8 *horizontal,
    const Vec8 *vertical
) {
    double maximum = 0.0;
    for (size_t index = 0; index < E8_ROOT_COUNT; ++index) {
        double x = dot8(&roots[index], horizontal);
        double y = dot8(&roots[index], vertical);
        double radius = hypot(x, y);
        if (radius > maximum) {
            maximum = radius;
        }
    }
    return maximum;
}

static int renderer_resize(Renderer *renderer, int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    size_t cell_count = (size_t)width * (size_t)height;
    if (cell_count > MAX_CELLS) {
        fprintf(stderr, "Terminal is too large: %dx%d exceeds %u cells\n",
                width, height, MAX_CELLS);
        return 0;
    }

    Cell *new_cells = realloc(renderer->cells, cell_count * sizeof(*new_cells));
    if (new_cells == NULL) {
        fputs("Unable to allocate render buffer\n", stderr);
        return 0;
    }
    renderer->cells = new_cells;
    renderer->width = width;
    renderer->height = height;

    /* One 24-bit ANSI color sequence per cell is the strict upper bound. */
    size_t output_capacity = cell_count * 28U + (size_t)height * 12U + 32U;
    char *new_output = realloc(renderer->output, output_capacity);
    if (new_output == NULL) {
        fputs("Unable to allocate terminal output buffer\n", stderr);
        return 0;
    }
    renderer->output = new_output;
    renderer->output_capacity = output_capacity;
    return 1;
}

static void add_energy(Renderer *renderer, int x, int y, float energy, float radius) {
    if (x < 0 || x >= renderer->width || y < 0 || y >= renderer->height) {
        return;
    }
    Cell *cell = &renderer->cells[(size_t)y * renderer->width + x];
    cell->energy += energy;
    cell->radius_sum += energy * radius;
}

static void splat(Renderer *renderer, double x, double y, float radius, float strength) {
    int left = (int)floor(x);
    int top = (int)floor(y);
    float dx = (float)(x - left);
    float dy = (float)(y - top);

    add_energy(renderer, left, top, strength * (1.0f - dx) * (1.0f - dy), radius);
    add_energy(renderer, left + 1, top, strength * dx * (1.0f - dy), radius);
    add_energy(renderer, left, top + 1, strength * (1.0f - dx) * dy, radius);
    add_energy(renderer, left + 1, top + 1, strength * dx * dy, radius);

    /* A faint halo keeps isolated roots readable at high resolutions. */
    add_energy(renderer, left - 1, top, strength * 0.10f, radius);
    add_energy(renderer, left + 2, top, strength * 0.10f, radius);
    add_energy(renderer, left, top - 1, strength * 0.07f, radius);
    add_energy(renderer, left, top + 2, strength * 0.07f, radius);
}

static void draw_ray(
    Renderer *renderer,
    double center_x,
    double center_y,
    double target_x,
    double target_y,
    float radius
) {
    double dx = target_x - center_x;
    double dy = target_y - center_y;
    int steps = (int)fmax(fabs(dx), fabs(dy));
    if (steps < 2) {
        return;
    }
    for (int step = 1; step < steps; ++step) {
        double progress = (double)step / steps;
        float strength = (float)(0.004 * progress * progress);
        add_energy(
            renderer,
            (int)lround(center_x + dx * progress),
            (int)lround(center_y + dy * progress),
            strength,
            radius * (float)progress
        );
    }
}

static int append_bytes(Renderer *renderer, size_t *used, const char *text, size_t length) {
    if (*used + length > renderer->output_capacity) {
        return 0;
    }
    memcpy(renderer->output + *used, text, length);
    *used += length;
    return 1;
}

static int append_format(
    Renderer *renderer,
    size_t *used,
    int red,
    int green,
    int blue
) {
    char sequence[32];
    int length = snprintf(sequence, sizeof(sequence), "\033[38;2;%d;%d;%dm",
                          red, green, blue);
    if (length < 0 || (size_t)length >= sizeof(sequence)) {
        return 0;
    }
    return append_bytes(renderer, used, sequence, (size_t)length);
}

static int compose_frame(Renderer *renderer, double phase, int color) {
    size_t used = 0;
    int previous_red = -1;
    int previous_green = -1;
    int previous_blue = -1;
    const size_t ramp_length = strlen(DENSITY_RAMP);

    if (!append_bytes(renderer, &used, "\033[H", 3)) {
        return 0;
    }

    for (int y = 0; y < renderer->height; ++y) {
        for (int x = 0; x < renderer->width; ++x) {
            const Cell *cell = &renderer->cells[(size_t)y * renderer->width + x];
            float normalized = 1.0f - expf(-cell->energy * 1.35f);
            size_t symbol_index = (size_t)lroundf(normalized * (float)(ramp_length - 1));
            if (symbol_index >= ramp_length) {
                symbol_index = ramp_length - 1;
            }

            if (color && symbol_index != 0) {
                float radius = cell->energy > 1e-6f
                    ? cell->radius_sum / cell->energy
                    : 0.0f;
                double hue = fmod(0.64 + 0.42 * radius + 0.08 * sin(phase), 1.0);
                double wave = 0.5 + 0.5 * sin(2.0 * PI * hue);
                double wave2 = 0.5 + 0.5 * sin(2.0 * PI * (hue + 1.0 / 3.0));
                double wave3 = 0.5 + 0.5 * sin(2.0 * PI * (hue + 2.0 / 3.0));
                int brightness = 95 + (int)(160.0f * normalized);
                int red = (int)(brightness * (0.28 + 0.72 * wave));
                int green = (int)(brightness * (0.28 + 0.72 * wave2));
                int blue = (int)(brightness * (0.28 + 0.72 * wave3));
                if (red != previous_red || green != previous_green || blue != previous_blue) {
                    if (!append_format(renderer, &used, red, green, blue)) {
                        return 0;
                    }
                    previous_red = red;
                    previous_green = green;
                    previous_blue = blue;
                }
            } else if (color && previous_red != 0) {
                if (!append_bytes(renderer, &used, "\033[39m", 5)) {
                    return 0;
                }
                previous_red = 0;
                previous_green = 0;
                previous_blue = 0;
            }

            if (!append_bytes(renderer, &used, &DENSITY_RAMP[symbol_index], 1)) {
                return 0;
            }
        }
        if (y + 1 < renderer->height
            && !append_bytes(renderer, &used, "\r\n", 2)) {
            return 0;
        }
    }
    if (color && !append_bytes(renderer, &used, "\033[0m", 4)) {
        return 0;
    }

    return fwrite(renderer->output, 1, used, stdout) == used;
}

static int render_frame(
    Renderer *renderer,
    const Vec8 roots[E8_ROOT_COUNT],
    const Vec8 *horizontal,
    const Vec8 *vertical,
    double radius_limit,
    double phase,
    const Options *options
) {
    size_t cell_count = (size_t)renderer->width * renderer->height;
    memset(renderer->cells, 0, cell_count * sizeof(*renderer->cells));

    double projected_x[E8_ROOT_COUNT];
    double projected_y[E8_ROOT_COUNT];
    double frame_radius_limit = radius_limit;
    double in_plane_angle = 0.22 * sin(phase) + 0.06 * sin(3.0 * phase);
    double in_plane_cos = cos(in_plane_angle);
    double in_plane_sin = sin(in_plane_angle);

    for (size_t index = 0; index < E8_ROOT_COUNT; ++index) {
        Vec8 root = roots[index];
        animate_root(&root, phase);
        double x = dot8(&root, horizontal);
        double y = dot8(&root, vertical);
        projected_x[index] = x * in_plane_cos - y * in_plane_sin;
        projected_y[index] = x * in_plane_sin + y * in_plane_cos;
        double radius = hypot(projected_x[index], projected_y[index]);
        if (radius > frame_radius_limit) {
            frame_radius_limit = radius;
        }
    }
    frame_radius_limit *= 1.015;

    double center_x = (renderer->width - 1) * 0.5;
    double center_y = (renderer->height - 1) * 0.5;
    double horizontal_radius = fmin(renderer->width * 0.465, renderer->height * 0.93);
    double vertical_radius = horizontal_radius * 0.5;
    double breathing = 0.91 + 0.075 * sin(2.0 * phase);

    for (size_t index = 0; index < E8_ROOT_COUNT; ++index) {
        double x = projected_x[index];
        double y = projected_y[index];
        float radius = (float)(hypot(x, y) / frame_radius_limit);
        double screen_x = center_x + x / frame_radius_limit * horizontal_radius * breathing;
        double screen_y = center_y - y / frame_radius_limit * vertical_radius * breathing;

        if (options->rays) {
            draw_ray(renderer, center_x, center_y, screen_x, screen_y, radius);
        }
        splat(renderer, screen_x, screen_y, radius, 1.65f);
    }

    return compose_frame(renderer, phase, options->color);
}

static double monotonic_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return 0.0;
    }
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void sleep_until(double deadline) {
    for (;;) {
        double remaining = deadline - monotonic_seconds();
        if (remaining <= 0.0 || !keep_running) {
            return;
        }
        struct timespec delay = {
            .tv_sec = (time_t)remaining,
            .tv_nsec = (long)((remaining - floor(remaining)) * 1000000000.0),
        };
        if (nanosleep(&delay, &delay) == 0 || errno != EINTR) {
            return;
        }
    }
}

static void print_usage(FILE *stream, const char *program) {
    fprintf(stream,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Animate the 240 roots of E8 in their Coxeter plane.\n"
        "\n"
        "  -f, --fps N       frame rate, 1..120 (default: 30)\n"
        "  -c, --cycle SEC   seamless cycle duration, 2..300 (default: 24)\n"
        "      --frames N    stop after N frames (useful for automation)\n"
        "      --no-color    disable 24-bit ANSI color\n"
        "      --no-rays     render roots without radial traces\n"
        "      --check       validate E8 roots and Coxeter geometry, then exit\n"
        "  -h, --help        show this help\n",
        program);
}

static int parse_long(const char *text, long minimum, long maximum, long *value) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int parse_double(const char *text, double minimum, double maximum, double *value) {
    char *end = NULL;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)
        || parsed < minimum || parsed > maximum) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int parse_options(int argc, char **argv, Options *options, int *check_only) {
    *options = (Options){
        .fps = 30,
        .cycle_seconds = 24.0,
        .frame_limit = 0,
        .color = 1,
        .rays = 1,
    };
    *check_only = 0;

    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            print_usage(stdout, argv[0]);
            exit(0);
        }
        if (strcmp(argument, "--no-color") == 0) {
            options->color = 0;
            continue;
        }
        if (strcmp(argument, "--no-rays") == 0) {
            options->rays = 0;
            continue;
        }
        if (strcmp(argument, "--check") == 0) {
            *check_only = 1;
            continue;
        }

        if (strcmp(argument, "-f") == 0 || strcmp(argument, "--fps") == 0) {
            long value;
            if (++index >= argc || !parse_long(argv[index], 1, 120, &value)) {
                fprintf(stderr, "Invalid FPS (expected 1..120)\n");
                return 0;
            }
            options->fps = (int)value;
            continue;
        }
        if (strcmp(argument, "-c") == 0 || strcmp(argument, "--cycle") == 0) {
            if (++index >= argc
                || !parse_double(argv[index], 2.0, 300.0, &options->cycle_seconds)) {
                fprintf(stderr, "Invalid cycle duration (expected 2..300 seconds)\n");
                return 0;
            }
            continue;
        }
        if (strcmp(argument, "--frames") == 0) {
            long value;
            if (++index >= argc || !parse_long(argv[index], 1, 1000000, &value)) {
                fprintf(stderr, "Invalid frame count (expected 1..1000000)\n");
                return 0;
            }
            options->frame_limit = (unsigned long)value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argument);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    Options options;
    int check_only;
    if (!parse_options(argc, argv, &options, &check_only)) {
        print_usage(stderr, argv[0]);
        return 2;
    }

    Vec8 roots[E8_ROOT_COUNT];
    Vec8 horizontal;
    Vec8 vertical;
    size_t root_count = generate_e8_roots(roots);
    if (!build_coxeter_plane(&horizontal, &vertical)
        || !validate_geometry(roots, root_count, &horizontal, &vertical)) {
        return 1;
    }
    if (check_only) {
        puts("OK: 240 E8 roots; orthonormal Coxeter plane; Coxeter order 30");
        return 0;
    }

    if (!isatty(STDOUT_FILENO)) {
        fputs("mandala requires an interactive terminal on stdout\n", stderr);
        return 1;
    }

    struct sigaction stop_action = {0};
    stop_action.sa_handler = handle_stop;
    sigemptyset(&stop_action.sa_mask);
    struct sigaction resize_action = {0};
    resize_action.sa_handler = handle_resize;
    sigemptyset(&resize_action.sa_mask);
    sigaction(SIGINT, &stop_action, NULL);
    sigaction(SIGTERM, &stop_action, NULL);
    sigaction(SIGHUP, &stop_action, NULL);
    sigaction(SIGWINCH, &resize_action, NULL);

    Renderer renderer = {0};
    struct winsize terminal = {0};
    double radius_limit = projected_radius_limit(roots, &horizontal, &vertical);
    double started_at = monotonic_seconds();
    double next_frame = started_at;
    unsigned long frame = 0;
    int exit_code = 0;

    if (atexit(restore_terminal) != 0) {
        fputs("Unable to register terminal cleanup\n", stderr);
        return 1;
    }
    fputs("\033[?1049h\033[?25l\033[2J", stdout);
    fflush(stdout);
    terminal_active = 1;

    while (keep_running && (options.frame_limit == 0 || frame < options.frame_limit)) {
        if (resize_pending) {
            resize_pending = 0;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &terminal) == -1
                || terminal.ws_col == 0 || terminal.ws_row == 0) {
                fputs("Unable to determine terminal size\n", stderr);
                exit_code = 1;
                break;
            }
            if (!renderer_resize(&renderer, terminal.ws_col, terminal.ws_row)) {
                exit_code = 1;
                break;
            }
            fputs("\033[2J", stdout);
        }

        double now = monotonic_seconds();
        double cycle_position = fmod(now - started_at, options.cycle_seconds)
            / options.cycle_seconds;
        double phase = 2.0 * PI * cycle_position;
        if (!render_frame(
                &renderer, roots, &horizontal, &vertical, radius_limit, phase, &options)) {
            fputs("Unable to write animation frame\n", stderr);
            exit_code = 1;
            break;
        }
        fflush(stdout);
        ++frame;

        next_frame += 1.0 / options.fps;
        if (next_frame < now - 1.0 / options.fps) {
            next_frame = now;
        }
        sleep_until(next_frame);
    }

    free(renderer.cells);
    free(renderer.output);
    restore_terminal();
    return exit_code;
}
