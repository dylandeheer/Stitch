// SPDX-License-Identifier: GPL-3.0-or-later
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void directory(const char* path) {
    struct stat st;
    if (mkdir(path, 0700) != 0 && (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))) {
        perror(path);
        exit(1);
    }
}

int main(int argc, char** argv) {
    char executable[PATH_MAX], resolved[PATH_MAX], payload[PATH_MAX], library[PATH_MAX];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) || !realpath(executable, resolved)) return 1;
    const char* bin = dirname(resolved);
    snprintf(payload, sizeof(payload), "%s/stitch-bin", bin);
    snprintf(library, sizeof(library), "%s/../Frameworks/libMoltenVK.dylib", bin);
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* entry = getpwuid(getuid());
        if (!entry) return 1;
        home = entry->pw_dir;
    }
    char root[PATH_MAX], path[PATH_MAX], default_game[PATH_MAX];
    snprintf(root, sizeof(root), "%s/Library/Application Support/Stitch", home);
    directory(root);
    strlcat(root, "/zelda-mac-v1", sizeof(root));
    directory(root);
    const char* folders[] = {"config", "data", "cache", "diagnostics"};
    const char* variables[] = {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"};
    for (unsigned i = 0; i < 4; ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, folders[i]);
        directory(path);
        if (i < 3) setenv(variables[i], path, 1);
    }
    snprintf(path, sizeof(path), "%s/diagnostics/launcher.log", root);
    int log = open(path, O_CREAT | O_APPEND | O_WRONLY, 0600);
    if (log >= 0) { dup2(log, STDOUT_FILENO); dup2(log, STDERR_FILENO); close(log); }
    // Apple startup changes cwd to the app's parent before selecting portable user data.
    snprintf(path, sizeof(path), "%s/../../..", bin);
    char parent[PATH_MAX];
    if (!realpath(path, parent)) return 1;
    strlcat(parent, "/user", sizeof(parent));
    struct stat st;
    if (lstat(parent, &st) == 0) {
        fprintf(stderr, "Stitch: move the conflicting portable user folder before launching: %s\n", parent);
        return 1;
    }
    const char* clear[] = {
        "QT_QPA_PLATFORM", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
        "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_INSERT_LIBRARIES",
        "SDL_AUDIODRIVER", "VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_INSTANCE_LAYERS",
        "MESA_VK_TRACE", "MESA_VK_TRACE_FRAME", "MESA_VK_TRACE_TRIGGER", "MESA_VK_TRACE_PER_SUBMIT",
        "RADV_THREAD_TRACE", "PERF_BUILDID_DIR", "EDEN_NX2_GPU_PROFILE", "EDEN_DKB_FRAME_TRACE",
        "EDEN_NX2_NO_MESH", "EDEN_NX2_NO_MESH_EMULATION", "EDEN_NX2_NO_TASK_EMULATION",
        "EDEN_NX2_FAST_DRAW_VERIFY", "EDEN_NX2_TEXTURE_MEMO_VERIFY", "EDEN_NX2_UPLOAD_TIMING",
        "STITCH_JIT_WARMUP", "STITCH_OOT_COMPARE_LINK", "STITCH_PAIR_TENSOR_COLUMNS",
        "STITCH_TENSOR_BASELINE_FILE", "STITCH_EARLY_READ_BASELINE_FILE", "STITCH_EARLY_READ_VERIFY",
        "STITCH_D16_ZERO_BASELINE_FILE", "STITCH_R32_ZERO_BASELINE_FILE", "STITCH_ATLAS_PREAD_BASELINE_FILE",
        "STITCH_ORDERED_FIFO_MACRO", "STITCH_ORDERED_FIFO_MODE_FILE", "STITCH_BOUNDED_MESH_TASK_LOOKUP",
        "STITCH_MESH_BASELINE_FILE", "STITCH_PRIVATE_MME_IMAGE", "EDEN_NX2_NUP_STAND_INS"
    };
    for (unsigned i = 0; i < sizeof(clear) / sizeof(*clear); ++i) unsetenv(clear[i]);
    setenv("QT_QPA_PLATFORM", "cocoa", 1);
    setenv("LIBVULKAN_PATH", library, 1);
    setenv("EDEN_NX2_SPLIT", "2", 1);
    setenv("EDEN_NX2_FENCE_BATCH", "1", 1);
    setenv("EDEN_NX2_FORCE_MESH_FALLBACK", "1", 1);
    setenv("EDEN_NX2_DEBUG_SERVER", "127.0.0.1:6546", 1);
    snprintf(path, sizeof(path), "%s/diagnostics", root);
    setenv("EDEN_NX2_DEBUG_DIR", path, 1);
    argv[0] = payload;
    if (argc == 1) {
        snprintf(path, sizeof(path), "%s/default-game.txt", root);
        FILE* file = fopen(path, "r");
        if (file && fgets(default_game, sizeof(default_game), file)) {
            fclose(file);
            default_game[strcspn(default_game, "\r\n")] = 0;
            if (access(default_game, R_OK) == 0) {
                char* args[] = {payload, "-g", default_game, NULL};
                execv(payload, args);
                perror("Stitch exec");
                return 1;
            }
        } else if (file) fclose(file);
    }
    execv(payload, argv);
    perror("Stitch exec");
    return 1;
}
