/*
 * Cobra Systems Language CLI Driver
 *
 * Copyright (c) 2026 The Cobra Project Authors.
 * All rights reserved.
 */

#include "../include/cobra.h"
#include "backend_ir/driver.h"
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>

static int run_process(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return status;
}

static bool process_succeeded(int status) {
    return status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool make_local_exec_path(const char *value, char *out, size_t capacity) {
    if (!value || !*value || !out || capacity == 0) return false;
    if (strchr(value, '/') != NULL) {
        return snprintf(out, capacity, "%s", value) < (int)capacity;
    }
    return snprintf(out, capacity, "./%s", value) < (int)capacity;
}

static bool ast_contains_tensor_intrinsic(ASTNode *node);
static bool ast_contains_parallel(ASTNode *node);
static bool ast_contains_collections(ASTNode *node);
static const char *parallel_runtime_path(void);
static const char *collections_runtime_path(void);
static const char *stackguard_runtime_path(void);

#define COBRA_MAX_IMPORTED_LIBRARIES 256
static char import_link_args[COBRA_MAX_IMPORTED_LIBRARIES][COBRA_MAX_TOKEN_TEXT + 8];

/* Convert `import c "libc.so.6" (...)` into a direct, shell-free linker
   argument. Bare names use GNU ld's exact-name form; paths are passed directly
   so local development libraries work too. */
static int append_import_libraries(ASTNode *program, const char **argv,
                                   int argc, int capacity) {
    int import_count = 0;
    if (!program) return argc;
    for (size_t i = 0; i < program->child_count; i++) {
        ASTNode *decl = program->children[i];
        if (decl->type != AST_IMPORT_DECL || decl->source_import || decl->name[0] == '\0') continue;
        if (decl->name[0] == '-') {
            fprintf(stderr, "[link] invalid C import library '%s'\n", decl->name);
            return -1;
        }
        if (import_count >= COBRA_MAX_IMPORTED_LIBRARIES || argc + 1 >= capacity) {
            fprintf(stderr, "[link] too many imported C libraries\n");
            return -1;
        }
        if (strchr(decl->name, '/')) {
            snprintf(import_link_args[import_count], sizeof(import_link_args[import_count]),
                     "%s", decl->name);
        } else {
            snprintf(import_link_args[import_count], sizeof(import_link_args[import_count]),
                     "-l:%s", decl->name);
        }
        argv[argc++] = import_link_args[import_count++];
    }
    return argc;
}
static bool host_supports_avx2(void);
static bool host_supports_fma(void);
static char *read_file_contents(const char *filepath);

static char *read_library_file(const char *name) {
    char local_path[512];
    snprintf(local_path, sizeof(local_path), "lib/%s", name);
    FILE *probe = fopen(local_path, "rb");
    if (probe) {
        fclose(probe);
        return read_file_contents(local_path);
    }
    const char *lib_path = getenv("COBRA_LIB_PATH");
    if (lib_path && *lib_path) {
        snprintf(local_path, sizeof(local_path), "%s/%s", lib_path, name);
        probe = fopen(local_path, "rb");
        if (probe) {
            fclose(probe);
            return read_file_contents(local_path);
        }
    }
    return NULL;
}

typedef struct {
    char active[128][PATH_MAX];
    size_t active_count;
    char loaded[128][PATH_MAX];
    size_t loaded_count;
} CobraModulePaths;

#define COBRA_MAX_SOURCE_SEGMENTS 512

typedef struct {
    size_t first_line;
    size_t last_line;
    char source_file[COBRA_MAX_SOURCE_PATH];
} CobraSourceSegment;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    CobraSourceSegment segments[COBRA_MAX_SOURCE_SEGMENTS];
    size_t segment_count;
} CobraSourceBuffer;

#define COBRA_MAX_DEPENDENCIES 64
#define COBRA_MAX_DEPENDENCY_PATH 512
#define COBRA_MAX_MANIFEST_LINE 512
#define COBRA_MAX_MANIFEST_BYTES (64 * 1024)

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    char path[COBRA_MAX_DEPENDENCY_PATH];
} CobraDependency;

typedef struct {
    bool found;
    char manifest_path[PATH_MAX];
    char project_root[PATH_MAX];
    char package_name[COBRA_MAX_IDENT_LEN];
    char package_version[COBRA_MAX_IDENT_LEN];
    CobraDependency dependencies[COBRA_MAX_DEPENDENCIES];
    size_t dependency_count;
} CobraProjectConfig;

static char *manifest_trim(char *text) {
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static void manifest_strip_comment(char *line) {
    bool quoted = false;
    bool escaped = false;
    for (char *p = line; *p; p++) {
        if (escaped) { escaped = false; continue; }
        if (*p == '\\') { escaped = true; continue; }
        if (*p == '\"') quoted = !quoted;
        else if (*p == '#' && !quoted) { *p = '\0'; return; }
    }
}

static bool manifest_assignment(char *line, char key[COBRA_MAX_IDENT_LEN],
                                char value[PATH_MAX]) {
    char *equals = strchr(line, '=');
    if (!equals) return false;
    *equals = '\0';
    char *key_text = manifest_trim(line);
    char *value_text = manifest_trim(equals + 1);
    size_t key_length = strlen(key_text);
    if (key_length == 0 || key_length >= COBRA_MAX_IDENT_LEN) return false;
    for (size_t i = 0; i < key_length; i++) {
        if (!(isalnum((unsigned char)key_text[i]) || key_text[i] == '_')) return false;
    }
    if (value_text[0] != '\"') return false;
    char *closing = NULL;
    for (char *p = value_text + 1; *p; p++) {
        if (*p == '\\') return false; /* paths and metadata are deliberately unescaped */
        if (*p == '\"') { closing = p; break; }
    }
    if (!closing || manifest_trim(closing + 1)[0] != '\0') return false;
    size_t value_length = (size_t)(closing - value_text - 1);
    if (value_length >= PATH_MAX) return false;
    memcpy(key, key_text, key_length + 1);
    memcpy(value, value_text + 1, value_length);
    value[value_length] = '\0';
    return true;
}

static bool path_is_within_root(const char *root, const char *path) {
    size_t root_length = strlen(root);
    if (strncmp(root, path, root_length) != 0) return false;
    return path[root_length] == '\0' || path[root_length] == '/';
}

static bool parse_project_manifest(const char *path, CobraProjectConfig *config) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Manifest Error: cannot open '%s'\n", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size < 0 || size > COBRA_MAX_MANIFEST_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Manifest Error: '%s' exceeds the %d-byte manifest limit\n",
                path, COBRA_MAX_MANIFEST_BYTES);
        fclose(file);
        return false;
    }

    char line[COBRA_MAX_MANIFEST_LINE];
    char section[32] = "";
    size_t line_number = 0;
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        if (!strchr(line, '\n') && !feof(file)) {
            fprintf(stderr, "Manifest Error: line %zu in '%s' is too long\n", line_number, path);
            fclose(file);
            return false;
        }
        manifest_strip_comment(line);
        char *text = manifest_trim(line);
        if (!*text) continue;
        if (text[0] == '[') {
            char *close = strchr(text, ']');
            if (!close || manifest_trim(close + 1)[0] != '\0') {
                fprintf(stderr, "Manifest Error: malformed section on line %zu in '%s'\n", line_number, path);
                fclose(file);
                return false;
            }
            *close = '\0';
            snprintf(section, sizeof(section), "%s", manifest_trim(text + 1));
            if (strcmp(section, "package") != 0 && strcmp(section, "dependencies") != 0) {
                fprintf(stderr, "Manifest Error: unknown section '[%s]' on line %zu in '%s'\n",
                        section, line_number, path);
                fclose(file);
                return false;
            }
            continue;
        }

        char key[COBRA_MAX_IDENT_LEN];
        char value[PATH_MAX];
        if (!manifest_assignment(text, key, value)) {
            fprintf(stderr, "Manifest Error: expected key = \"value\" on line %zu in '%s'\n",
                    line_number, path);
            fclose(file);
            return false;
        }
        if (section[0] == '\0') {
            fprintf(stderr, "Manifest Error: key '%s' must appear inside [package] or [dependencies] on line %zu in '%s'\n",
                    key, line_number, path);
            fclose(file);
            return false;
        }
        if (strcmp(section, "package") == 0) {
            if (strcmp(key, "name") == 0) snprintf(config->package_name, sizeof(config->package_name), "%s", value);
            else if (strcmp(key, "version") == 0) snprintf(config->package_version, sizeof(config->package_version), "%s", value);
            else {
                fprintf(stderr, "Manifest Error: unknown package key '%s' on line %zu in '%s'\n",
                        key, line_number, path);
                fclose(file);
                return false;
            }
        } else if (strcmp(section, "dependencies") == 0) {
            if (config->dependency_count >= COBRA_MAX_DEPENDENCIES) {
                fprintf(stderr, "Manifest Error: too many dependencies in '%s'\n", path);
                fclose(file);
                return false;
            }
            if (!value[0] || value[0] == '/') {
                fprintf(stderr, "Manifest Error: dependency '%s' must use a non-empty relative path\n", key);
                fclose(file);
                return false;
            }
            for (size_t i = 0; i < config->dependency_count; i++) {
                if (strcmp(config->dependencies[i].name, key) == 0) {
                    fprintf(stderr, "Manifest Error: duplicate dependency '%s'\n", key);
                    fclose(file);
                    return false;
                }
            }
            snprintf(config->dependencies[config->dependency_count].name,
                     sizeof(config->dependencies[config->dependency_count].name), "%s", key);
            if (strlen(value) >= COBRA_MAX_DEPENDENCY_PATH) {
                fprintf(stderr, "Manifest Error: dependency path '%s' is too long\n", key);
                fclose(file);
                return false;
            }
            snprintf(config->dependencies[config->dependency_count].path,
                     sizeof(config->dependencies[config->dependency_count].path), "%s", value);
            config->dependency_count++;
        }
    }
    fclose(file);
    return true;
}

static bool discover_project_manifest(const char *source_path, CobraProjectConfig *config) {
    memset(config, 0, sizeof(*config));
    char canonical[PATH_MAX];
    if (!realpath(source_path, canonical)) return true;
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", canonical);
    char *slash = strrchr(directory, '/');
    if (!slash) snprintf(directory, sizeof(directory), ".");
    else if (slash == directory) directory[1] = '\0';
    else *slash = '\0';

    while (1) {
        char candidate[PATH_MAX];
        if (snprintf(candidate, sizeof(candidate), "%s/cobra.toml", directory) >= (int)sizeof(candidate)) return true;
        FILE *probe = fopen(candidate, "rb");
        if (probe) {
            fclose(probe);
            config->found = true;
            snprintf(config->manifest_path, sizeof(config->manifest_path), "%s", candidate);
            snprintf(config->project_root, sizeof(config->project_root), "%s", directory);
            return parse_project_manifest(candidate, config);
        }
        if (strcmp(directory, "/") == 0 || strcmp(directory, ".") == 0) break;
        char *parent_slash = strrchr(directory, '/');
        if (!parent_slash) break;
        if (parent_slash == directory) directory[1] = '\0';
        else *parent_slash = '\0';
    }
    return true;
}

static bool source_buffer_append(CobraSourceBuffer *buffer, const char *text,
                                 const char *source_file) {
    if (buffer->segment_count >= COBRA_MAX_SOURCE_SEGMENTS) return false;
    size_t length = strlen(text);
    if (buffer->length + length + 2 < buffer->length) return false;
    if (buffer->length + length + 2 > buffer->capacity) {
        size_t next = buffer->capacity ? buffer->capacity : 4096;
        while (next < buffer->length + length + 2) {
            if (next > SIZE_MAX / 2) return false;
            next *= 2;
        }
        char *grown = realloc(buffer->data, next);
        if (!grown) return false;
        buffer->data = grown;
        buffer->capacity = next;
    }
    size_t first_line = 1;
    for (size_t i = 0; i < buffer->length; i++) {
        if (buffer->data[i] == '\n') first_line++;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length++] = '\n';
    buffer->data[buffer->length] = '\0';
    if (buffer->segment_count < COBRA_MAX_SOURCE_SEGMENTS) {
        CobraSourceSegment *segment = &buffer->segments[buffer->segment_count++];
        segment->first_line = first_line;
        segment->last_line = first_line;
        for (size_t i = 0; i < length; i++) {
            if (text[i] == '\n') segment->last_line++;
        }
        snprintf(segment->source_file, sizeof(segment->source_file), "%s",
                 source_file && *source_file ? source_file : "<source>");
    }
    return true;
}

static bool annotate_source_locations(ASTNode *node, const CobraSourceBuffer *buffer) {
    if (!node || !buffer) return true;
    if (node->source_line > 0) {
        size_t line = (size_t)node->source_line;
        bool mapped = false;
        for (size_t i = 0; i < buffer->segment_count; i++) {
            const CobraSourceSegment *segment = &buffer->segments[i];
            if (line >= segment->first_line && line <= segment->last_line) {
                node->source_line = (int)(line - segment->first_line + 1);
                snprintf(node->source_file, sizeof(node->source_file), "%s", segment->source_file);
                mapped = true;
                break;
            }
        }
        if (!mapped) {
            fprintf(stderr, "%s:%d:%d: error: unable to map syntax node to a loaded source segment\n",
                    node->source_file[0] ? node->source_file : "<source>",
                    node->source_line, node->source_col > 0 ? node->source_col : 1);
            return false;
        }
    }
    for (size_t i = 0; i < node->child_count; i++) {
        if (!annotate_source_locations(node->children[i], buffer)) return false;
    }
    return true;
}

static void print_module_import_chain(const CobraModulePaths *paths) {
    if (!paths || paths->active_count == 0) return;
    fputs("Module import chain:", stderr);
    for (size_t i = 0; i < paths->active_count; i++) {
        fprintf(stderr, " %s%s", i == 0 ? "" : "-> ", paths->active[i]);
    }
    fputc('\n', stderr);
}

static bool module_active_in(const CobraModulePaths *paths, const char *path) {
    for (size_t i = 0; i < paths->active_count; i++) {
        if (strcmp(paths->active[i], path) == 0) return true;
    }
    return false;
}

static bool module_loaded_in(const CobraModulePaths *paths, const char *path) {
    for (size_t i = 0; i < paths->loaded_count; i++) {
        if (strcmp(paths->loaded[i], path) == 0) return true;
    }
    return false;
}

static bool module_path_push(CobraModulePaths *paths, const char *path) {
    if (paths->active_count >= 128 || module_active_in(paths, path)) return false;
    snprintf(paths->active[paths->active_count++], PATH_MAX, "%s", path);
    return true;
}

static void module_path_pop(CobraModulePaths *paths, const char *path) {
    if (paths->active_count > 0 && strcmp(paths->active[paths->active_count - 1], path) == 0) {
        paths->active_count--;
    }
}

static bool module_mark_loaded(CobraModulePaths *paths, const char *path) {
    if (module_loaded_in(paths, path)) return true;
    if (paths->loaded_count >= 128) return false;
    snprintf(paths->loaded[paths->loaded_count++], PATH_MAX, "%s", path);
    return true;
}

static bool resolve_module_path(const char *importer, const char *requested,
                                const CobraProjectConfig *config,
                                char resolved[PATH_MAX]) {
    char candidate[PATH_MAX];
    if (requested[0] == '/') {
        if (snprintf(candidate, sizeof(candidate), "%s", requested) < (int)sizeof(candidate) &&
            realpath(candidate, resolved)) {
            if (config && config->found && !path_is_within_root(config->project_root, resolved)) {
                fprintf(stderr, "Module Error: absolute import '%s' escapes project root '%s'\n",
                        requested, config->project_root);
                return false;
            }
            return true;
        }
    } else {
        char directory[PATH_MAX];
        snprintf(directory, sizeof(directory), "%s", importer);
        char *slash = strrchr(directory, '/');
        if (slash) *slash = '\0';
        else snprintf(directory, sizeof(directory), ".");
        if (snprintf(candidate, sizeof(candidate), "%s/%s", directory, requested) < (int)sizeof(candidate) &&
            realpath(candidate, resolved)) {
            if (config && config->found && !path_is_within_root(config->project_root, resolved)) {
                fprintf(stderr, "Module Error: import '%s' escapes project root '%s'\n",
                        requested, config->project_root);
                return false;
            }
            return true;
        }
    }

    if (config && config->found) {
        const char *separator = strchr(requested, '/');
        size_t prefix_length = separator ? (size_t)(separator - requested) : strlen(requested);
        for (size_t i = 0; i < config->dependency_count; i++) {
            if (strlen(config->dependencies[i].name) != prefix_length ||
                strncmp(config->dependencies[i].name, requested, prefix_length) != 0) continue;
            const char *suffix = separator ? separator + 1 : "";
            if (snprintf(candidate, sizeof(candidate), "%s/%s%s%s", config->project_root,
                         config->dependencies[i].path, *suffix ? "/" : "", suffix) >= (int)sizeof(candidate)) return false;
            if (realpath(candidate, resolved)) {
                if (!path_is_within_root(config->project_root, resolved)) {
                    fprintf(stderr, "Module Error: dependency '%s' escapes project root '%s'\n",
                            requested, config->project_root);
                    return false;
                }
                return true;
            }
        }
    }

    const char *lib_path = getenv("COBRA_LIB_PATH");
    if (lib_path && *lib_path &&
        snprintf(candidate, sizeof(candidate), "%s/%s", lib_path, requested) < (int)sizeof(candidate) &&
        realpath(candidate, resolved)) return true;
    return false;
}

static bool load_cobra_module(const char *path, const CobraProjectConfig *config,
                              CobraModulePaths *paths, CobraSourceBuffer *output) {
    char canonical[PATH_MAX];
    if (!realpath(path, canonical)) {
        fprintf(stderr, "Module Error: cannot resolve source module '%s'\n", path);
        return false;
    }
    if (module_active_in(paths, canonical)) {
        fprintf(stderr, "Module Error: cyclic source import involving '%s'\n", canonical);
        print_module_import_chain(paths);
        return false;
    }
    if (module_loaded_in(paths, canonical)) return true;
    if (!module_path_push(paths, canonical)) {
        fprintf(stderr, "Module Error: source module graph is too deep '%s'\n", canonical);
        return false;
    }

    char *source = read_file_contents(canonical);
    if (!source) {
        module_path_pop(paths, canonical);
        return false;
    }
    Parser import_parser;
    parser_init_with_file(&import_parser, source, canonical);
    ASTNode *imports = parser_parse_program(&import_parser);
    for (size_t i = 0; i < imports->child_count; i++) {
        ASTNode *decl = imports->children[i];
        if (decl->type != AST_IMPORT_DECL || !decl->source_import) continue;
        char dependency[PATH_MAX];
        if (!resolve_module_path(canonical, decl->name, config, dependency)) {
            fprintf(stderr, "%s:%d:%d: error: cannot resolve module '%s' imported by '%s'\n",
                    decl->source_file[0] ? decl->source_file : canonical,
                    decl->source_line > 0 ? decl->source_line : 1,
                    decl->source_col > 0 ? decl->source_col : 1,
                    decl->name, canonical);
            print_module_import_chain(paths);
            ast_free(imports);
            free(source);
            module_path_pop(paths, canonical);
            return false;
        }
        if (!load_cobra_module(dependency, config, paths, output)) {
            ast_free(imports);
            free(source);
            module_path_pop(paths, canonical);
            return false;
        }
    }
    ast_free(imports);
    bool appended = source_buffer_append(output, source, canonical);
    free(source);
    module_path_pop(paths, canonical);
    if (!appended || !module_mark_loaded(paths, canonical)) {
        if (!appended) fprintf(stderr, "Module Error: source buffer exhausted or source segment table is full while loading '%s'\n", canonical);
        else fprintf(stderr, "Module Error: too many loaded source modules\n");
        return false;
    }
    return true;
}

static char *read_file_contents(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", filepath);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "Error: Cannot seek source file '%s'\n", filepath);
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Cannot determine source size for '%s'\n", filepath);
        fclose(f);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    if (read_bytes != (size_t)size && ferror(f)) {
        fprintf(stderr, "Error: Cannot read source file '%s'\n", filepath);
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[read_bytes] = '\0';
    fclose(f);
    return buf;
}

static bool write_new_project_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "wbx");
    if (!file) {
        fprintf(stderr, "[init] cannot create '%s': %s\n", path, strerror(errno));
        return false;
    }
    size_t length = strlen(contents);
    bool written = fwrite(contents, 1, length, file) == length;
    if (fclose(file) != 0) written = false;
    if (!written) {
        fprintf(stderr, "[init] cannot write '%s'\n", path);
        remove(path);
        return false;
    }
    return true;
}

static bool make_project_directory(const char *directory, bool *created,
                                    char rollback_stop[PATH_MAX]) {
    char path[PATH_MAX];
    if (!directory || !*directory || strlen(directory) >= sizeof(path)) return false;
    snprintf(path, sizeof(path), "%s", directory);
    *created = false;
    rollback_stop[0] = '\0';
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') path[--length] = '\0';
    size_t start = path[0] == '/' ? 1 : 0;
    for (size_t i = start; i <= length; i++) {
        if (path[i] != '/' && path[i] != '\0') continue;
        char saved = path[i];
        path[i] = '\0';
        if (path[0] != '\0' && strcmp(path, ".") != 0) {
            struct stat status;
            if (stat(path, &status) == 0) {
                if (!S_ISDIR(status.st_mode)) return false;
                if (!*created) snprintf(rollback_stop, PATH_MAX, "%s", path);
            } else if (errno == ENOENT) {
                if (mkdir(path, 0755) != 0 && errno != EEXIST) return false;
                if (!*created) {
                    /* The closest existing ancestor is the rollback boundary. */
                    char parent[PATH_MAX];
                    snprintf(parent, sizeof(parent), "%s", path);
                    char *slash = strrchr(parent, '/');
                    if (!slash) snprintf(rollback_stop, PATH_MAX, ".");
                    else if (slash == parent) snprintf(rollback_stop, PATH_MAX, "/");
                    else { *slash = '\0'; snprintf(rollback_stop, PATH_MAX, "%s", parent); }
                }
                *created = true;
            } else {
                return false;
            }
        }
        path[i] = saved;
    }
    return true;
}

static void rollback_project_directories(const char *directory, const char *stop) {
    char path[PATH_MAX];
    if (!directory || !*directory || !stop) return;
    if (snprintf(path, sizeof(path), "%s", directory) >= (int)sizeof(path)) return;
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') path[--length] = '\0';
    while (strcmp(path, stop) != 0 && strcmp(path, "/") != 0 && strcmp(path, ".") != 0) {
        if (rmdir(path) != 0) break;
        char *slash = strrchr(path, '/');
        if (!slash) break;
        if (slash == path) path[1] = '\0';
        else *slash = '\0';
    }
}

static bool project_name_from_directory(const char *directory,
                                        char name[COBRA_MAX_IDENT_LEN]) {
    char absolute[PATH_MAX];
    const char *value = directory;
    if (strcmp(directory, ".") == 0 || strcmp(directory, "./") == 0) {
        if (!getcwd(absolute, sizeof(absolute))) return false;
        value = absolute;
    }
    size_t length = strlen(value);
    while (length > 1 && value[length - 1] == '/') length--;
    const char *start = value;
    for (size_t i = length; i > 0; i--) {
        if (value[i - 1] == '/') {
            start = value + i;
            break;
        }
    }
    if (!*start || (size_t)(start - value) >= length) return false;
    size_t output = 0;
    for (const char *p = start; p < value + length && output + 1 < COBRA_MAX_IDENT_LEN; p++) {
        unsigned char c = (unsigned char)*p;
        name[output++] = (isalnum(c) || c == '_') ? (char)c : '_';
    }
    if (output == 0) return false;
    name[output] = '\0';
    if (isdigit((unsigned char)name[0])) {
        if (output + 5 >= COBRA_MAX_IDENT_LEN) return false;
        memmove(name + 5, name, output + 1);
        memcpy(name, "cobra", 5);
    }
    return true;
}

static bool run_init(const char *directory) {
    struct stat status;
    bool directory_created = false;
    char rollback_stop[PATH_MAX] = "";
    if (stat(directory, &status) == 0) {
        if (!S_ISDIR(status.st_mode)) {
            fprintf(stderr, "[init] '%s' is not a directory\n", directory);
            return false;
        }
    } else if (errno == ENOENT) {
        if (!make_project_directory(directory, &directory_created, rollback_stop)) {
            fprintf(stderr, "[init] cannot create directory '%s': %s\n",
                    directory, strerror(errno));
            if (directory_created) rollback_project_directories(directory, rollback_stop);
            return false;
        }
    } else {
        fprintf(stderr, "[init] cannot inspect '%s': %s\n", directory, strerror(errno));
        return false;
    }

    char manifest_path[PATH_MAX];
    char source_path[PATH_MAX];
    if (snprintf(manifest_path, sizeof(manifest_path), "%s/cobra.toml", directory) >= (int)sizeof(manifest_path) ||
        snprintf(source_path, sizeof(source_path), "%s/main.cb", directory) >= (int)sizeof(source_path)) {
        fprintf(stderr, "[init] project path is too long\n");
        goto init_failure;
    }
    if (access(manifest_path, F_OK) == 0 || access(source_path, F_OK) == 0) {
        fprintf(stderr, "[init] refusing to overwrite an existing cobra.toml or main.cb in '%s'\n", directory);
        goto init_failure;
    }

    char project_name[COBRA_MAX_IDENT_LEN];
    if (!project_name_from_directory(directory, project_name)) {
        fprintf(stderr, "[init] could not derive a project name from '%s'\n", directory);
        goto init_failure;
    }
    char manifest[1024];
    int manifest_length = snprintf(manifest, sizeof(manifest),
        "[package]\nname = \"%s\"\nversion = \"0.1.0\"\n\n[dependencies]\n",
        project_name);
    if (manifest_length < 0 || (size_t)manifest_length >= sizeof(manifest) ||
        !write_new_project_file(manifest_path, manifest)) goto init_failure;

    static const char starter[] =
        "def main() -> i64: {\n"
        "    return 0\n"
        "}\n";
    if (!write_new_project_file(source_path, starter)) {
        remove(manifest_path);
        goto init_failure;
    }
    printf("[init] created Cobra project '%s'\n", project_name);
    printf("[init] manifest: %s\n", manifest_path);
    printf("[init] entry:    %s\n", source_path);
    return true;

init_failure:
    if (directory_created) rollback_project_directories(directory, rollback_stop);
    return false;
}

static void print_usage(void) {
    printf("Cobra Systems Compiler & Ecosystem CLI (v%s)\n", COBRA_VERSION_STRING);
    printf("Usage:\n");
    printf("  cobra init [directory]\n");
    printf("  cobra check <file.cb>\n");
    printf("  cobra build <file.cb> [-o binary] [--target=win64|wasm32|arm64] [--backend=native|direct] [--no-gpu]\n");
    printf("  cobra run <file.cb> [--backend=native|direct]\n");
    printf("  cobra test <file.cb>\n");
    printf("  cobra bench <file.cb> [--warmup N] [--runs N]\n");
    printf("  cobra fmt <file.cb>\n");
    printf("  cobra repl\n");
    printf("  cobra update\n");
    printf("  cobra emit-asm <file.cb> [-o output.s] [--target=win64|wasm32|arm64]\n");
}

static void run_repl(void) {
    printf("Cobra Interactive REPL (v%s)\n", COBRA_VERSION_STRING);
    printf("Type Cobra expressions or statements. Type 'exit' to quit.\n");
    printf("----------------------------------------------------------\n");

    char line_buf[512];
    while (1) {
        printf("cobra> ");
        fflush(stdout);
        if (!fgets(line_buf, sizeof(line_buf), stdin)) break;
        
        // Remove trailing newline
        line_buf[strcspn(line_buf, "\r\n")] = 0;
        if (strcmp(line_buf, "exit") == 0 || strcmp(line_buf, "quit") == 0) break;
        if (strlen(line_buf) == 0) continue;

        // Wrap expression in function and evaluate
        char code[1024];
        snprintf(code, sizeof(code), "def main(): {\n  %s\n  return 0\n}\n", line_buf);

        Parser parser;
        parser_init(&parser, code);
        ASTNode *program = parser_parse_program(&parser);

        CobraIR repl_ir;
        bool repl_has_tensor = ast_contains_tensor_intrinsic(program);
        if (repl_has_tensor && (!host_supports_avx2() || !host_supports_fma())) {
            fprintf(stderr, "[repl] f32 tensor intrinsics require AVX2 and FMA support on the host CPU\n");
            ast_free(program);
            continue;
        }
        if (cobra_ir_build(program, &repl_ir) &&
            codegen_generate_assembly(program, ".repl_tmp.s", TARGET_LINUX_X86_64)) {
            const char *runtime = ast_contains_parallel(program) ? parallel_runtime_path() : NULL;
            if (ast_contains_parallel(program) && !runtime) {
                fprintf(stderr, "[repl] @parallel runtime not found; set COBRA_LIB_PATH or run from the Cobra source tree\\n");
                remove(".repl_tmp.s");
                ast_free(program);
                continue;
            }
            const char *build_argv[300];
            int build_argc = 0;
            build_argv[build_argc++] = "gcc";
            build_argv[build_argc++] = "-no-pie";
            build_argv[build_argc++] = ".repl_tmp.s";
            if (runtime) {
                build_argv[build_argc++] = runtime;
                build_argv[build_argc++] = "-pthread";
            }
            const char *collections_runtime = ast_contains_collections(program) ? collections_runtime_path() : NULL;
            if (ast_contains_collections(program) && !collections_runtime) {
                fprintf(stderr, "[repl] collections runtime not found\\n");
                remove(".repl_tmp.s");
                ast_free(program);
                continue;
            }
            if (collections_runtime) build_argv[build_argc++] = collections_runtime;
            const char *stackguard_runtime = stackguard_runtime_path();
            if (stackguard_runtime) build_argv[build_argc++] = stackguard_runtime;
            build_argc = append_import_libraries(program, build_argv, build_argc, 300);
            if (build_argc < 0) {
                remove(".repl_tmp.s");
                ast_free(program);
                continue;
            }
            build_argv[build_argc++] = "-lm";
            build_argv[build_argc++] = "-o";
            build_argv[build_argc++] = ".repl_tmp_bin";
            build_argv[build_argc] = NULL;
            if (process_succeeded(run_process(build_argv))) {
                const char *run_argv[] = {"./.repl_tmp_bin", NULL};
                (void)run_process(run_argv);
                remove(".repl_tmp_bin");
            }
            remove(".repl_tmp.s");
        }
        ast_free(program);
    }
}

static bool ast_contains_compute(ASTNode *node) {
    if (!node) return false;
    if (node->type == AST_COMPUTE_BLOCK) return true;
    for (size_t i = 0; i < node->child_count; i++) {
        if (ast_contains_compute(node->children[i])) return true;
    }
    return false;
}

/* A bare `for i in len(x):` or `for i in range(len(x)):` loop is now
   auto-dispatched to the parallel worker pool by codegen when its body
   proves index-pure over an f32 buffer, without needing an explicit
   `@parallel:` block. This walk has no type information, so it can't
   confirm f32-ness the way codegen does; it over-approximates on loop
   shape alone, which only ever costs linking the (pthread-only, no heavy
   deps) parallel runtime for a program that turns out not to need it. */
static bool ast_for_loop_could_auto_parallelize(ASTNode *node) {
    if (node->type != AST_FOR_LOOP || node->child_count < 1) return false;
    ASTNode *target = node->children[0];
    if (target->type == AST_LEN_EXPR) return true;
    if (target->type == AST_FUNC_CALL && !strcmp(target->name, "range") &&
        target->child_count == 1 && target->children[0]->type == AST_LEN_EXPR) return true;
    return false;
}

static bool ast_contains_parallel(ASTNode *node) {
    if (!node) return false;
    if (node->type == AST_PARALLEL_BLOCK) return true;
    if (ast_for_loop_could_auto_parallelize(node)) return true;
    for (size_t i = 0; i < node->child_count; i++) {
        if (ast_contains_parallel(node->children[i])) return true;
    }
    return false;
}

static const char *collections_runtime_path(void) {
    static char installed_path[512];
    if (access("runtime/cobra_collections.c", R_OK) == 0) return "runtime/cobra_collections.c";
    const char *lib_path = getenv("COBRA_LIB_PATH");
    if (lib_path && *lib_path) {
        snprintf(installed_path, sizeof(installed_path), "%s/cobra_collections.c", lib_path);
        if (access(installed_path, R_OK) == 0) return installed_path;
    }
    return NULL;
}

/* Unlike the other optional runtime helpers above, the stack guard is linked
   into every build/test/bench binary unconditionally: any program can
   recurse, and installing the handler costs one ELF constructor call at
   startup, not a per-function or per-call cost. Returns NULL (silently
   skipped by callers) rather than erroring when the source tree/install
   layout doesn't have it, so an unusual environment degrades to the old
   raw-segfault behavior instead of failing the build. */
static const char *stackguard_runtime_path(void) {
    static char installed_path[512];
    if (access("runtime/cobra_stackguard.c", R_OK) == 0) return "runtime/cobra_stackguard.c";
    const char *lib_path = getenv("COBRA_LIB_PATH");
    if (lib_path && *lib_path) {
        snprintf(installed_path, sizeof(installed_path), "%s/cobra_stackguard.c", lib_path);
        if (access(installed_path, R_OK) == 0) return installed_path;
    }
    return NULL;
}

static bool ast_contains_precision(ASTNode *node) {
    if (!node) return false;
    if (node->type == AST_FUNC_CALL &&
        (!strcmp(node->name, "pack_f16") || !strcmp(node->name, "unpack_f16"))) return true;
    for (size_t i = 0; i < node->child_count; i++) {
        if (ast_contains_precision(node->children[i])) return true;
    }
    return false;
}

static const char *precision_runtime_path(void) {
    static char installed_path[512];
    if (access("runtime/cobra_precision.c", R_OK) == 0) return "runtime/cobra_precision.c";
    const char *lib_path = getenv("COBRA_LIB_PATH");
    if (lib_path && *lib_path) {
        snprintf(installed_path, sizeof(installed_path), "%s/cobra_precision.c", lib_path);
        if (access(installed_path, R_OK) == 0) return installed_path;
    }
    return NULL;
}

static const char *gpu_runtime_path(void) {
    static char installed_path[512];
    if (access("runtime/cobra_gpu.c", R_OK) == 0) return "runtime/cobra_gpu.c";
    const char *lib_path = getenv("COBRA_LIB_PATH");
    if (lib_path && *lib_path) {
        snprintf(installed_path, sizeof(installed_path), "%s/cobra_gpu.c", lib_path);
        if (access(installed_path, R_OK) == 0) return installed_path;
    }
    return NULL;
}

bool cobra_gpu_lower_function(const ASTNode *program, const ASTNode *fn, FILE *out);
bool cobra_gpu_lower_backward(const ASTNode *program, const ASTNode *fn, FILE *out,
                               int *out_buffer_count, char out_buffer_names[][COBRA_MAX_IDENT_LEN],
                               int *out_scalar_count, char out_scalar_names[][COBRA_MAX_IDENT_LEN],
                               CobraTypeKind *out_scalar_types, char *out_output_buf);

static bool ast_has_gpu_kernel(ASTNode *program) {
    if (!program) return false;
    for (size_t i = 0; i < program->child_count; i++) {
        ASTNode *fn = program->children[i];
        if (fn->type == AST_FUNCTION && fn->target_device == TARGET_DEV_GPU_KERNEL) return true;
    }
    return false;
}

static const char *gpu_glsl_scalar_c_type(CobraTypeKind k) {
    if (k == COBRA_TYPE_F32 || k == COBRA_TYPE_F64) return "float";
    return "int32_t";
}

/* Lowers every @gpu-tagged top-level function in `program` to SPIR-V (via
   glslangValidator) and writes a generated C source file with the embedded
   SPIR-V plus one wrapper function per kernel, matching the ABI
   emit_gpu_kernel_call in codegen.c produces at each call site:
   (float *buf, int64_t count, <scalars>...). Returns the generated file's
   path, or NULL on failure (with a message already printed to stderr). */
static const char *build_gpu_kernels_file(ASTNode *program, const char *output_binary) {
    static char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s_gpu_kernels.c", output_binary);
    FILE *out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "[codegen] failed to create %s\n", out_path); return NULL; }
    fprintf(out, "#include <stdint.h>\n#include <stddef.h>\n");
    fprintf(out, "extern int64_t cobra_gpu_run_kernel_n(const uint32_t *spirv, size_t spirv_words, float **buffers, const int64_t *lens, int64_t nbuffers, const void *push_blob, size_t push_blob_size);\n");
    fprintf(out, "extern int64_t cobra_gpu_run_kernel_resident(const uint32_t *spirv, size_t spirv_words, const int64_t *handles, int64_t nbuffers, const void *push_blob, size_t push_blob_size);\n");
    fprintf(out, "extern int64_t cobra_gpu_resident_len(int64_t handle);\n\n");

    bool ok = true;
    for (size_t i = 0; i < program->child_count && ok; i++) {
        ASTNode *fn = program->children[i];
        if (fn->type != AST_FUNCTION || fn->target_device != TARGET_DEV_GPU_KERNEL) continue;

        char glsl_path[512], spv_path[512];
        snprintf(glsl_path, sizeof(glsl_path), "%s_%s.comp", output_binary, fn->name);
        snprintf(spv_path, sizeof(spv_path), "%s_%s.spv", output_binary, fn->name);
        FILE *glsl = fopen(glsl_path, "w");
        if (!glsl) { fprintf(stderr, "[codegen] failed to create %s\n", glsl_path); ok = false; break; }
        bool lowered = cobra_gpu_lower_function(program, fn, glsl);
        fclose(glsl);
        if (!lowered) { remove(glsl_path); ok = false; break; }

        const char *spv_argv[] = {"glslangValidator", "-V", glsl_path, "-o", spv_path, NULL};
        int status = run_process(spv_argv);
        remove(glsl_path);
        if (!process_succeeded(status)) {
            fprintf(stderr, "[codegen] @gpu kernel '%s': glslangValidator failed (is it installed?)\n", fn->name);
            ok = false; break;
        }

        FILE *spv = fopen(spv_path, "rb");
        if (!spv) { fprintf(stderr, "[codegen] failed to read %s\n", spv_path); ok = false; break; }
        fseek(spv, 0, SEEK_END);
        long spv_size = ftell(spv);
        fseek(spv, 0, SEEK_SET);
        if (spv_size <= 0 || spv_size % 4 != 0) {
            fprintf(stderr, "[codegen] @gpu kernel '%s': malformed SPIR-V output\n", fn->name);
            fclose(spv); remove(spv_path); ok = false; break;
        }
        size_t word_count = (size_t)spv_size / 4;
        uint32_t *words = malloc((size_t)spv_size);
        if (!words || fread(words, 1, (size_t)spv_size, spv) != (size_t)spv_size) {
            fprintf(stderr, "[codegen] @gpu kernel '%s': failed to read SPIR-V\n", fn->name);
            fclose(spv); free(words); remove(spv_path); ok = false; break;
        }
        fclose(spv);
        remove(spv_path);

        fprintf(out, "static const uint32_t __cobra_gpu_spirv_%s[] = {", fn->name);
        for (size_t w = 0; w < word_count; w++) fprintf(out, "%s%uu", w ? "," : "", words[w]);
        fprintf(out, "};\n");
        free(words);

        char buf_names[8][COBRA_MAX_IDENT_LEN]; int buf_count = 0;
        char scalar_names[16][COBRA_MAX_IDENT_LEN]; CobraTypeKind scalar_types[16]; int scalar_count = 0;
        for (size_t p = 0; p < fn->child_count; p++) {
            ASTNode *param = fn->children[p];
            if (param->type != AST_PARAM) continue;
            if (param->declared_type == COBRA_TYPE_SLICE_F32) {
                if (buf_count < 8) snprintf(buf_names[buf_count++], COBRA_MAX_IDENT_LEN, "%s", param->name);
            } else if (scalar_count < 16) {
                snprintf(scalar_names[scalar_count], COBRA_MAX_IDENT_LEN, "%s", param->name);
                scalar_types[scalar_count] = param->declared_type;
                scalar_count++;
            }
        }

        /* Push-constant layout matches what gpu_lower.c emitted in the GLSL
           source: one uint length per buffer (declaration order), then each
           scalar in declaration order. */
        fprintf(out, "int64_t %s(float **__bufs, int64_t *__lens", fn->name);
        for (int s = 0; s < scalar_count; s++)
            fprintf(out, ", %s %s", gpu_glsl_scalar_c_type(scalar_types[s]), scalar_names[s]);
        fprintf(out, ") {\n");
        fprintf(out, "    struct { ");
        for (int b = 0; b < buf_count; b++) fprintf(out, "uint32_t l%d; ", b);
        for (int s = 0; s < scalar_count; s++) fprintf(out, "%s f%d; ", gpu_glsl_scalar_c_type(scalar_types[s]), s);
        fprintf(out, "} __push;\n");
        for (int b = 0; b < buf_count; b++) fprintf(out, "    __push.l%d = (uint32_t)__lens[%d];\n", b, b);
        for (int s = 0; s < scalar_count; s++) fprintf(out, "    __push.f%d = %s;\n", s, scalar_names[s]);
        fprintf(out, "    return cobra_gpu_run_kernel_n(__cobra_gpu_spirv_%s, %zu, __bufs, __lens, %d, &__push, sizeof(__push));\n",
                fn->name, word_count, buf_count);
        fprintf(out, "}\n\n");

        /* Resident fast path: `<kernel>_gpu(...)`. Buffer parameters become
           i64 handles (from gpu_alloc_f32) in the same declaration position;
           lengths come from the resident table via cobra_gpu_resident_len,
           since the wrapper only has handles on hand, not raw counts. */
        bool param_is_buf[32]; char param_names[32][COBRA_MAX_IDENT_LEN]; CobraTypeKind param_types[32];
        int param_count = 0;
        for (size_t p = 0; p < fn->child_count && param_count < 32; p++) {
            ASTNode *param = fn->children[p];
            if (param->type != AST_PARAM) continue;
            bool is_buf = param->declared_type == COBRA_TYPE_SLICE_F32;
            param_is_buf[param_count] = is_buf;
            snprintf(param_names[param_count], COBRA_MAX_IDENT_LEN, "%s", param->name);
            param_types[param_count] = is_buf ? COBRA_TYPE_I64 : param->declared_type;
            param_count++;
        }

        fprintf(out, "int64_t %s_gpu(", fn->name);
        for (int p = 0; p < param_count; p++)
            fprintf(out, "%s%s %s", p ? ", " : "",
                    param_is_buf[p] ? "int64_t" : gpu_glsl_scalar_c_type(param_types[p]), param_names[p]);
        fprintf(out, ") {\n");
        fprintf(out, "    int64_t __handles[%d] = {", buf_count);
        { int bi = 0; for (int p = 0; p < param_count; p++) if (param_is_buf[p]) fprintf(out, "%s%s", bi++ ? ", " : "", param_names[p]); }
        fprintf(out, "};\n");
        fprintf(out, "    struct { ");
        for (int b = 0; b < buf_count; b++) fprintf(out, "uint32_t l%d; ", b);
        for (int s = 0; s < scalar_count; s++) fprintf(out, "%s f%d; ", gpu_glsl_scalar_c_type(scalar_types[s]), s);
        fprintf(out, "} __push;\n");
        for (int b = 0; b < buf_count; b++) fprintf(out, "    __push.l%d = (uint32_t)cobra_gpu_resident_len(__handles[%d]);\n", b, b);
        for (int s = 0; s < scalar_count; s++) fprintf(out, "    __push.f%d = %s;\n", s, scalar_names[s]);
        fprintf(out, "    return cobra_gpu_run_kernel_resident(__cobra_gpu_spirv_%s, %zu, __handles, %d, &__push, sizeof(__push));\n",
                fn->name, word_count, buf_count);
        fprintf(out, "}\n\n");

        /* Reverse-mode gradient kernel, if this kernel is elementwise
           enough to qualify (see cobra_gpu_lower_backward). Not every @gpu
           kernel does - this is attempted opportunistically and skipped
           (not an error) when it doesn't apply. */
        {
            char bglsl_path[512], bspv_path[512];
            snprintf(bglsl_path, sizeof(bglsl_path), "%s_%s_backward.comp", output_binary, fn->name);
            snprintf(bspv_path, sizeof(bspv_path), "%s_%s_backward.spv", output_binary, fn->name);
            FILE *bglsl = fopen(bglsl_path, "w");
            int gb_count = 0, gs_count = 0;
            char gb_names[8][COBRA_MAX_IDENT_LEN];
            char gs_names[16][COBRA_MAX_IDENT_LEN];
            CobraTypeKind gs_types[16];
            char output_buf_name[COBRA_MAX_IDENT_LEN];
            bool eligible = bglsl && cobra_gpu_lower_backward(program, fn, bglsl, &gb_count, gb_names,
                                                               &gs_count, gs_names, gs_types, output_buf_name);
            if (bglsl) fclose(bglsl);
            if (eligible) {
                const char *bspv_argv[] = {"glslangValidator", "-V", bglsl_path, "-o", bspv_path, NULL};
                int bstatus = run_process(bspv_argv);
                remove(bglsl_path);
                if (process_succeeded(bstatus)) {
                    FILE *bspv = fopen(bspv_path, "rb");
                    if (bspv) {
                        fseek(bspv, 0, SEEK_END);
                        long bspv_size = ftell(bspv);
                        fseek(bspv, 0, SEEK_SET);
                        uint32_t *bwords = (bspv_size > 0 && bspv_size % 4 == 0) ? malloc((size_t)bspv_size) : NULL;
                        if (bwords && fread(bwords, 1, (size_t)bspv_size, bspv) == (size_t)bspv_size) {
                            size_t bword_count = (size_t)bspv_size / 4;
                            fprintf(out, "static const uint32_t __cobra_gpu_spirv_%s_backward[] = {", fn->name);
                            for (size_t w = 0; w < bword_count; w++) fprintf(out, "%s%uu", w ? "," : "", bwords[w]);
                            fprintf(out, "};\n");

                            /* ABI: (float **bufs, int64_t *lens, <original scalars>), matching
                               emit_gpu_backward_call in codegen.c exactly - bufs/lens together
                               hold [grad_out, <original buffers>, <grad_<buffer> outputs>,
                               <grad_<scalar>_partial outputs>], all sharing the same dispatch
                               length (bufs[0]/lens[0] is grad_out, whose length is the kernel's
                               own dispatch domain). */
                            int total_bufs = 1 + 2 * gb_count + gs_count;
                            fprintf(out, "int64_t %s_backward(float **__bufs, int64_t *__lens", fn->name);
                            for (int s = 0; s < gs_count; s++) fprintf(out, ", %s %s", gpu_glsl_scalar_c_type(gs_types[s]), gs_names[s]);
                            fprintf(out, ") {\n");
                            fprintf(out, "    struct { uint32_t klen;");
                            for (int s = 0; s < gs_count; s++) fprintf(out, " float f%d;", s);
                            fprintf(out, " } __push;\n");
                            fprintf(out, "    __push.klen = (uint32_t)__lens[0];\n");
                            for (int s = 0; s < gs_count; s++) fprintf(out, "    __push.f%d = %s;\n", s, gs_names[s]);
                            fprintf(out, "    return cobra_gpu_run_kernel_n(__cobra_gpu_spirv_%s_backward, %zu, __bufs, __lens, %d, &__push, sizeof(__push));\n",
                                    fn->name, bword_count, total_bufs);
                            fprintf(out, "}\n\n");
                        }
                        free(bwords);
                        fclose(bspv);
                    }
                }
                remove(bspv_path);
            } else {
                remove(bglsl_path);
            }
        }
    }
    fclose(out);
    if (!ok) { remove(out_path); return NULL; }
    return out_path;
}

static bool ast_contains_gpu(ASTNode *node) {
    if (!node) return false;
    if (node->type == AST_FUNC_CALL &&
        (!strcmp(node->name, "gpu_available") || !strcmp(node->name, "gpu_device_count") ||
         !strcmp(node->name, "gpu_selftest") || !strcmp(node->name, "gpu_should_dispatch") ||
         /* matmul_f32/dense_f32/relu_f32/sum_f32/mean_f32/max_f32 all
            auto-dispatch to GPU under the hood (see emit_gemm/emit_relu/
            emit_reduce in codegen.c), so the runtime must be linked whenever
            these appear, even though the source never names a gpu_* call. */
         !strcmp(node->name, "matmul_f32") || !strcmp(node->name, "dense_f32") ||
         !strcmp(node->name, "relu_f32") || !strcmp(node->name, "sum_f32") ||
         !strcmp(node->name, "mean_f32") || !strcmp(node->name, "max_f32") ||
         !strcmp(node->name, "gpu_alloc_f32") || !strcmp(node->name, "gpu_upload_f32") ||
         !strcmp(node->name, "gpu_download_f32") || !strcmp(node->name, "gpu_free_resident") ||
         !strcmp(node->name, "gpu_batch_begin") || !strcmp(node->name, "gpu_batch_end") ||
         !strcmp(node->name, "matmul_f32_backward"))) return true;
    for (size_t i = 0; i < node->child_count; i++) {
        if (ast_contains_gpu(node->children[i])) return true;
    }
    return false;
}

static bool ast_contains_collections(ASTNode *node) {
    if (!node) return false;
    if (node->type == AST_DICT_LITERAL || node->type == AST_DICT_ENTRY) return true;
    if (node->type == AST_VAR_DECL || node->type == AST_PARAM) {
        if (node->declared_type == COBRA_TYPE_LIST || node->declared_type == COBRA_TYPE_DICT) return true;
    }
    if (node->type == AST_MEMBERSHIP || node->type == AST_COMPREHENSION) return true;
    if (node->type == AST_FUNC_CALL &&
        (!strcmp(node->name, "append") || !strcmp(node->name, "set") ||
         !strcmp(node->name, "get") || !strcmp(node->name, "has") ||
         !strcmp(node->name, "delete") || !strcmp(node->name, "pop"))) return true;
    for (size_t i = 0; i < node->child_count; i++) if (ast_contains_collections(node->children[i])) return true;
    return false;
}

static const char *parallel_runtime_path(void) {
    static char installed_path[512];
    if (access("runtime/cobra_parallel.c", R_OK) == 0) return "runtime/cobra_parallel.c";
    const char *lib_path = getenv("COBRA_LIB_PATH");
    if (lib_path && *lib_path) {
        snprintf(installed_path, sizeof(installed_path), "%s/cobra_parallel.c", lib_path);
        if (access(installed_path, R_OK) == 0) return installed_path;
    }
    return NULL;
}

static bool ast_contains_tensor_intrinsic(ASTNode *node) {
    if (!node) return false;
    if (node->type == AST_FUNC_CALL &&
        (strcmp(node->name, "alloc_f32") == 0 ||
         strcmp(node->name, "fill_f32") == 0 ||
         strcmp(node->name, "relu_f32") == 0 ||
         strcmp(node->name, "matmul_f32") == 0 ||
         strcmp(node->name, "dense_f32") == 0 ||
         strcmp(node->name, "sum_f32") == 0 ||
         strcmp(node->name, "mean_f32") == 0 ||
         strcmp(node->name, "max_f32") == 0 ||
         strcmp(node->name, "exp_f32") == 0 ||
         strcmp(node->name, "sqrt_f32") == 0 ||
         strcmp(node->name, "tanh_f32") == 0 ||
         strcmp(node->name, "log_f32") == 0 ||
         strcmp(node->name, "pow_f32") == 0)) {
        return true;
    }
    for (size_t i = 0; i < node->child_count; i++) {
        if (ast_contains_tensor_intrinsic(node->children[i])) return true;
    }
    return false;
}

/* Allocation of an f32 buffer is portable. Tensor operations require the
   AVX2/FMA kernel contract and cannot be emitted by --cpu=portable. The
   composed standard libraries are present in every AST, so this check follows
   only functions reachable from non-NN roots instead of scanning every NN
   helper and rejecting ordinary portable programs. */
static bool is_tensor_kernel_call(const ASTNode *node) {
    if (!node || node->type != AST_FUNC_CALL) return false;
    if (strcmp(node->name, "alloc_f32") == 0) return false;
    return strcmp(node->name, "fill_f32") == 0 || strcmp(node->name, "relu_f32") == 0 ||
           strcmp(node->name, "matmul_f32") == 0 || strcmp(node->name, "dense_f32") == 0 ||
           strcmp(node->name, "sum_f32") == 0 || strcmp(node->name, "mean_f32") == 0 ||
           strcmp(node->name, "max_f32") == 0;
}

static ASTNode *find_ast_function(ASTNode *program, const char *name) {
    if (!program || !name) return NULL;
    for (size_t i = 0; i < program->child_count; i++) {
        ASTNode *child = program->children[i];
        if (child->type == AST_FUNCTION && strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

static bool is_nn_library_function(const ASTNode *node) {
    return node && node->type == AST_FUNCTION &&
           strstr(node->source_file, "lib/nn.cb") != NULL;
}

static bool ast_function_reaches_tensor_kernel(ASTNode *program, ASTNode *function,
                                               ASTNode **stack, size_t depth);

static bool ast_node_reaches_tensor_kernel(ASTNode *program, ASTNode *node,
                                           ASTNode **stack, size_t depth) {
    if (!node) return false;
    if (node->declared_type == COBRA_TYPE_TENSOR_F32 ||
        node->value_type == COBRA_TYPE_TENSOR_F32) return true;
    if (node->type == AST_FUNC_CALL) {
        if (is_tensor_kernel_call(node)) return true;
        ASTNode *callee = find_ast_function(program, node->name);
        if (callee && ast_function_reaches_tensor_kernel(program, callee, stack, depth)) return true;
    }
    for (size_t i = 0; i < node->child_count; i++) {
        if (ast_node_reaches_tensor_kernel(program, node->children[i], stack, depth)) return true;
    }
    return false;
}

static bool ast_function_reaches_tensor_kernel(ASTNode *program, ASTNode *function,
                                               ASTNode **stack, size_t depth) {
    if (!function) return false;
    if (depth >= 128) return true;
    for (size_t i = 0; i < depth; i++) {
        if (stack[i] == function) return false;
    }
    stack[depth] = function;
    for (size_t i = 0; i < function->child_count; i++) {
        if (ast_node_reaches_tensor_kernel(program, function->children[i], stack, depth + 1)) return true;
    }
    return false;
}

static bool ast_contains_tensor_kernel(ASTNode *program) {
    if (!program) return false;
    ASTNode *stack[128];
    for (size_t i = 0; i < program->child_count; i++) {
        ASTNode *function = program->children[i];
        if (function->type != AST_FUNCTION || is_nn_library_function(function)) continue;
        if (ast_function_reaches_tensor_kernel(program, function, stack, 0)) return true;
    }
    return false;
}

static bool host_supports_avx2(void) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

static bool host_supports_fma(void) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("fma");
#else
    return false;
#endif
}

static bool native_test_cpu_supports_compute(ASTNode *program, const char *context) {
    bool has_compute = false;
    for (size_t i = 0; i < program->child_count; i++) {
        ASTNode *function = program->children[i];
        if (function->type == AST_FUNCTION &&
            strncmp(function->name, "test_", 5) == 0 &&
            ast_contains_compute(function)) {
            has_compute = true;
            break;
        }
    }
    if (has_compute && !host_supports_avx2()) {
        fprintf(stderr, "%s requires AVX2 support on the host CPU\n", context);
        return false;
    }
    return true;
}

#define COBRA_BENCH_MAX_SAMPLES 100000UL

static char benchmark_asm_path[128];
static char benchmark_binary_path[128];
static volatile sig_atomic_t benchmark_interrupted = 0;

static void benchmark_signal_handler(int signal_number) {
    (void)signal_number;
    benchmark_interrupted = 1;
}

static void cleanup_benchmark_artifacts(void) {
    if (benchmark_asm_path[0]) remove(benchmark_asm_path);
    if (benchmark_binary_path[0]) remove(benchmark_binary_path);
    benchmark_asm_path[0] = '\0';
    benchmark_binary_path[0] = '\0';
}

static bool parse_benchmark_count(const char *text, size_t *out, const char *option) {
    if (!text || !*text || text[0] == '-') {
        fprintf(stderr, "Error: %s requires a non-negative integer <= %lu\n", option, COBRA_BENCH_MAX_SAMPLES);
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value > COBRA_BENCH_MAX_SAMPLES) {
        fprintf(stderr, "Error: %s requires a non-negative integer <= %lu\n", option, COBRA_BENCH_MAX_SAMPLES);
        return false;
    }
    *out = (size_t)value;
    return true;
}

static int compare_doubles(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static bool benchmark_process(const char *binary_path, double *elapsed_seconds) {
    struct timespec start;
    if (benchmark_interrupted) return false;
    struct timespec end;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        execl(binary_path, binary_path, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    for (;;) {
        pid_t waited = waitpid(pid, &status, 0);
        if (waited == pid) break;
        if (waited < 0 && errno == EINTR && benchmark_interrupted) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            return false;
        }
        if (waited < 0) return false;
    }
    if (benchmark_interrupted) return false;
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;

    *elapsed_seconds = (double)(end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
    return true;
}

static bool run_benchmark(ASTNode *program, const char *source_path,
                          bool has_parallel, size_t warmups, size_t runs) {
    snprintf(benchmark_asm_path, sizeof(benchmark_asm_path), ".cobra_bench_%ld.s", (long)getpid());
    snprintf(benchmark_binary_path, sizeof(benchmark_binary_path), "./.cobra_bench_%ld", (long)getpid());
    benchmark_interrupted = 0;
    signal(SIGINT, benchmark_signal_handler);
    signal(SIGTERM, benchmark_signal_handler);
    atexit(cleanup_benchmark_artifacts);

    if (!codegen_generate_assembly(program, benchmark_asm_path, TARGET_LINUX_X86_64)) {
        fprintf(stderr, "[bench] Native assembly generation failed for '%s'\n", source_path);
        cleanup_benchmark_artifacts();
        return false;
    }

    const char *runtime = has_parallel ? parallel_runtime_path() : NULL;
    const char *collections_runtime = ast_contains_collections(program) ? collections_runtime_path() : NULL;
    if (has_parallel && !runtime) {
        fprintf(stderr, "[bench] @parallel runtime not found; set COBRA_LIB_PATH or run from the Cobra source tree\\n");
        cleanup_benchmark_artifacts();
        return false;
    }
    if (ast_contains_collections(program) && !collections_runtime) {
        fprintf(stderr, "[bench] collections runtime not found\\n");
        cleanup_benchmark_artifacts();
        return false;
    }

    const char *build_argv[300];
    int build_argc = 0;
    build_argv[build_argc++] = "gcc";
    build_argv[build_argc++] = "-no-pie";
    build_argv[build_argc++] = benchmark_asm_path;
    if (runtime) {
        build_argv[build_argc++] = runtime;
        build_argv[build_argc++] = "-pthread";
    }
    if (collections_runtime) build_argv[build_argc++] = collections_runtime;
    const char *stackguard_runtime = stackguard_runtime_path();
    if (stackguard_runtime) build_argv[build_argc++] = stackguard_runtime;
    build_argc = append_import_libraries(program, build_argv, build_argc, 300);
    if (build_argc < 0) {
        cleanup_benchmark_artifacts();
        return false;
    }
    build_argv[build_argc++] = "-lm";
    build_argv[build_argc++] = "-o";
    build_argv[build_argc++] = benchmark_binary_path;
    build_argv[build_argc] = NULL;
    if (!process_succeeded(run_process(build_argv))) {
        fprintf(stderr, "[bench] Native benchmark link failed for '%s'\n", source_path);
        cleanup_benchmark_artifacts();
        return false;
    }

    printf("[bench] workload: %s\n", source_path);
    printf("[bench] warmups: %zu, samples: %zu\n", warmups, runs);
    printf("[bench] timing: native process wall time (startup included)\n");

    double elapsed = 0.0;
    for (size_t i = 0; i < warmups; i++) {
        if (!benchmark_process(benchmark_binary_path, &elapsed)) {
            fprintf(stderr, "[bench] correctness failure during warmup %zu; benchmark aborted\n", i + 1);
            cleanup_benchmark_artifacts();
            return false;
        }
    }

    double *samples = (double *)malloc(sizeof(double) * runs);
    if (!samples) {
        fprintf(stderr, "[bench] could not allocate sample storage\n");
        cleanup_benchmark_artifacts();
        return false;
    }
    for (size_t i = 0; i < runs; i++) {
        if (!benchmark_process(benchmark_binary_path, &samples[i])) {
            fprintf(stderr, "[bench] correctness failure during sample %zu; benchmark aborted\n", i + 1);
            free(samples);
            cleanup_benchmark_artifacts();
            return false;
        }
    }

    qsort(samples, runs, sizeof(double), compare_doubles);
    double total = 0.0;
    for (size_t i = 0; i < runs; i++) total += samples[i];
    size_t p95_index = (runs * 95 + 99) / 100;
    if (p95_index == 0) p95_index = 1;
    if (p95_index > runs) p95_index = runs;
    printf("[bench] correctness: %zu/%zu samples exited successfully\n", runs, runs);
    printf("[bench] min:    %.3f ms\n", samples[0] * 1000.0);
    printf("[bench] median: %.3f ms\n", samples[runs / 2] * 1000.0);
    printf("[bench] p95:    %.3f ms\n", samples[p95_index - 1] * 1000.0);
    printf("[bench] max:    %.3f ms\n", samples[runs - 1] * 1000.0);
    printf("[bench] mean:   %.3f ms\n", (total / (double)runs) * 1000.0);

    free(samples);
    cleanup_benchmark_artifacts();
    return true;
}

static bool run_native_tests(ASTNode *program, const char *source_path) {
    /* The runner is generated as a tiny POSIX process supervisor. */
    char asm_path[128];
    char runner_path[128];
    char binary_path[128];
    snprintf(asm_path, sizeof(asm_path), ".cobra_test_%ld.s", (long)getpid());
    snprintf(runner_path, sizeof(runner_path), ".cobra_test_%ld.c", (long)getpid());
    snprintf(binary_path, sizeof(binary_path), ".cobra_test_%ld", (long)getpid());

    size_t test_count = 0;
    for (size_t i = 0; i < program->child_count; i++) {
        ASTNode *function = program->children[i];
        if (function->type == AST_FUNCTION && strncmp(function->name, "test_", 5) == 0) {
            for (size_t j = 0; j < function->child_count; j++) {
                if (function->children[j]->type == AST_PARAM) {
                    fprintf(stderr, "[test] Native tests must have no parameters: %s()\n", function->name);
                    return false;
                }
            }
            if (function->declared_type == COBRA_TYPE_VOID ||
                function->declared_type == COBRA_TYPE_F32 ||
                function->declared_type == COBRA_TYPE_F64 ||
                function->declared_type == COBRA_TYPE_V256) {
                fprintf(stderr, "[test] Native tests must return an integer status: %s()\n", function->name);
                return false;
            }
            test_count++;
        }
    }
    if (test_count == 0) {
        fprintf(stderr, "[test] No test_ functions found\n");
        return false;
    }

    if (!codegen_generate_test_assembly(program, asm_path, TARGET_LINUX_X86_64)) {
        fprintf(stderr, "[test] Native assembly generation failed for '%s'\n", source_path);
        remove(asm_path);
        return false;
    }

    FILE *runner = fopen(runner_path, "w");
    if (!runner) {
        fprintf(stderr, "[test] Could not create native test runner\n");
        remove(asm_path);
        return false;
    }

    fprintf(runner, "#include <stdio.h>\n");
    fprintf(runner, "#include <stdlib.h>\n");
    fprintf(runner, "#include <sys/types.h>\n");
    fprintf(runner, "#include <sys/wait.h>\n");
    fprintf(runner, "#include <unistd.h>\n");
    fprintf(runner, "#include <signal.h>\n\n");
    for (size_t i = 0; i < program->child_count; i++) {
        ASTNode *function = program->children[i];
        if (function->type == AST_FUNCTION && strncmp(function->name, "test_", 5) == 0) {
            fprintf(runner, "extern int %s(void);\n", function->name);
        }
    }
    fprintf(runner, "typedef int (*CobraTest)(void);\n");
    fprintf(runner, "int main(void) {\n");
    fprintf(runner, "    CobraTest tests[] = {");
    bool first = true;
    for (size_t i = 0; i < program->child_count; i++) {
        ASTNode *function = program->children[i];
        if (function->type == AST_FUNCTION && strncmp(function->name, "test_", 5) == 0) {
            fprintf(runner, "%s%s", first ? "" : ", ", function->name);
            first = false;
        }
    }
    fprintf(runner, "};\n");
    fprintf(runner, "    const char *names[] = {");
    first = true;
    for (size_t i = 0; i < program->child_count; i++) {
        ASTNode *function = program->children[i];
        if (function->type == AST_FUNCTION && strncmp(function->name, "test_", 5) == 0) {
            fputs(first ? "" : ", ", runner);
            fputc('"', runner);
            fputs(function->name, runner);
            fputc('"', runner);
            first = false;
        }
    }
    fprintf(runner, "};\n");
    fprintf(runner, "    int passed = 0, failed = 0;\n");
    fprintf(runner, "    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {\n");
    fprintf(runner, "        fflush(NULL);\n");
    fprintf(runner, "        pid_t pid = fork();\n");
    fprintf(runner, "        if (pid == 0) { int result = tests[i](); exit(result == 0 ? 0 : 1); }\n");
    fprintf(runner, "        if (pid < 0) { failed++; continue; }\n");
    fprintf(runner, "        int status = 0; int waited = 0;\n");
    fprintf(runner, "        int wait_error = 0; int finished = 0;\n");
    fprintf(runner, "        while (waited < 500) {\n");
    fprintf(runner, "            pid_t done = waitpid(pid, &status, WNOHANG);\n");
    fprintf(runner, "            if (done == pid) { finished = 1; break; }\n");
    fprintf(runner, "            if (done < 0) { wait_error = 1; break; }\n");
    fprintf(runner, "            usleep(10000); waited++;\n");
    fprintf(runner, "        }\n");
    fprintf(runner, "        int timed_out = 0;\n");
    fprintf(runner, "        if (waited >= 500) { kill(pid, SIGKILL); waitpid(pid, &status, 0); finished = 1; timed_out = 1; }\n");
    fprintf(runner, "        if (!wait_error && finished && !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) { printf(");
    fputc('"', runner);
    fputs("  PASS: %s()", runner);
    fputc('"', runner);
    fprintf(runner, ", names[i]); putchar(10); passed++; }\n");
    fprintf(runner, "        else if (timed_out) { printf(");
    fputc('"', runner);
    fputs("  FAIL: %s() (timed out after 5s)", runner);
    fputc('"', runner);
    fprintf(runner, ", names[i]); putchar(10); failed++; }\n");
    fprintf(runner, "        else { printf(");
    fputc('"', runner);
    fputs("  FAIL: %s()", runner);
    fputc('"', runner);
    fprintf(runner, ", names[i]); putchar(10); failed++; }\n");
    fprintf(runner, "    }\n");
    fprintf(runner, "    puts(");
    fputc('"', runner);
    fputs("-----------------------------------------", runner);
    fputc('"', runner);
    fprintf(runner, ");\n");
    fprintf(runner, "    printf(");
    fputc('"', runner);
    fputs("Result: %d passed, %d failed", runner);
    fputc('"', runner);
    fprintf(runner, ", passed, failed); putchar(10);\n");
    fprintf(runner, "    return failed == 0 ? 0 : 1;\n");
    fprintf(runner, "}\n");
    fclose(runner);

    const char *runtime = ast_contains_parallel(program) ? parallel_runtime_path() : NULL;
    const char *collections_runtime = ast_contains_collections(program) ? collections_runtime_path() : NULL;
    if (ast_contains_parallel(program) && !runtime) {
        fprintf(stderr, "[test] @parallel runtime not found; set COBRA_LIB_PATH or run from the Cobra source tree\n");
        remove(asm_path);
        remove(runner_path);
        return false;
    }
    if (ast_contains_collections(program) && !collections_runtime) {
        fprintf(stderr, "[test] collections runtime not found\n");
        remove(asm_path);
        remove(runner_path);
        return false;
    }
    /* matmul_f32/dense_f32/relu_f32/sum_f32/mean_f32/max_f32 auto-dispatch
       to GPU under the hood (see emit_gemm/emit_relu/emit_reduce in
       codegen.c), so any program that reaches them - directly or, as here,
       transitively through the always-linked lib/nn.cb prelude's
       conv2d_f32/dense_relu_f32/etc - needs the GPU runtime linked even in
       `cobra test`, exactly like the `cobra build` path already does. */
    bool program_has_gpu = ast_contains_gpu(program);
    const char *gpu_runtime = program_has_gpu ? gpu_runtime_path() : NULL;
    if (program_has_gpu && !gpu_runtime) {
        fprintf(stderr, "[test] gpu runtime not found; set COBRA_LIB_PATH or run from the Cobra source tree\n");
        remove(asm_path);
        remove(runner_path);
        return false;
    }
    const char *build_argv[300];
    int build_argc = 0;
    build_argv[build_argc++] = "gcc";
    build_argv[build_argc++] = "-no-pie";
    build_argv[build_argc++] = asm_path;
    build_argv[build_argc++] = runner_path;
    if (runtime) {
        build_argv[build_argc++] = runtime;
        build_argv[build_argc++] = "-pthread";
    }
    if (collections_runtime) build_argv[build_argc++] = collections_runtime;
    if (gpu_runtime) {
        build_argv[build_argc++] = gpu_runtime;
        build_argv[build_argc++] = "-ldl";
    }
    const char *stackguard_runtime = stackguard_runtime_path();
    if (stackguard_runtime) build_argv[build_argc++] = stackguard_runtime;
    build_argc = append_import_libraries(program, build_argv, build_argc, 300);
    if (build_argc < 0) {
        remove(asm_path);
        remove(runner_path);
        return false;
    }
    build_argv[build_argc++] = "-lm";
    build_argv[build_argc++] = "-o";
    build_argv[build_argc++] = binary_path;
    build_argv[build_argc] = NULL;
    int build_status = run_process(build_argv);
    if (!process_succeeded(build_status)) {
        fprintf(stderr, "[test] Native test link failed for '%s'\n", source_path);
        remove(asm_path);
        remove(runner_path);
        remove(binary_path);
        return false;
    }

    char run_path[256];
    if (!make_local_exec_path(binary_path, run_path, sizeof(run_path))) {
        remove(asm_path);
        remove(runner_path);
        remove(binary_path);
        return false;
    }
    const char *run_argv[] = {run_path, NULL};
    int run_status = run_process(run_argv);
    bool passed = process_succeeded(run_status);

    remove(asm_path);
    remove(runner_path);
    remove(binary_path);
    return passed;
}

static void run_update(void) {
    printf("[update] Checking GitHub (Aaron-Savron/Cobra) for latest release...\n");
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        fprintf(stderr, "Error: Could not create update pipe.\n");
        return;
    }
    pid_t downloader = fork();
    if (downloader == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        const char *args[] = {"curl", "-fsSL", "https://raw.githubusercontent.com/Aaron-Savron/Cobra/main/install.sh", NULL};
        execvp(args[0], (char *const *)args);
        _exit(127);
    }
    if (downloader < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        fprintf(stderr, "Error: Could not start update downloader.\n");
        return;
    }

    pid_t installer = fork();
    if (installer == 0) {
        dup2(pipe_fds[0], STDIN_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        const char *args[] = {"sh", NULL};
        execvp(args[0], (char *const *)args);
        _exit(127);
    }
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    if (installer < 0) {
        kill(downloader, SIGTERM);
        waitpid(downloader, NULL, 0);
        fprintf(stderr, "Error: Could not start update installer.\n");
        return;
    }

    int downloader_status = 0, installer_status = 0;
    waitpid(downloader, &downloader_status, 0);
    waitpid(installer, &installer_status, 0);
    if (process_succeeded(downloader_status) && process_succeeded(installer_status)) {
        printf("[update] Cobra compiler successfully updated to latest GitHub release!\n");
    } else {
        fprintf(stderr, "Error: Auto-update failed. Please check network connection or GitHub access.\n");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *command = argv[1];

    if (strcmp(command, "repl") == 0) {
        run_repl();
        return 0;
    }

    if (strcmp(command, "update") == 0) {
        run_update();
        return 0;
    }

    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(command, "init") == 0) {
        if (argc > 3) {
            fprintf(stderr, "Error: cobra init accepts at most one directory\n");
            return 1;
        }
        return run_init(argc == 3 ? argv[2] : ".") ? 0 : 1;
    }

    if (argc < 3) {
        fprintf(stderr, "Error: Missing source file path for command '%s'\n", command);
        print_usage();
        return 1;
    }

    const char *source_path = argv[2];
    const char *output_binary = "output";
    TargetPlatform target = TARGET_LINUX_X86_64;
    size_t bench_warmups = 2;
    size_t bench_runs = 10;
    bool opt_vectorize = true;
    bool portable_cpu = false;
    bool use_isolated_backend = false;
    bool use_object_emitter = false;
    bool use_gpu = true;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_binary = argv[++i];
        } else if (strcmp(argv[i], "--no-gpu") == 0) {
            use_gpu = false;
        } else if (strcmp(argv[i], "-O0") == 0) {
            opt_vectorize = false;
        } else if (strcmp(argv[i], "-O1") == 0 || strcmp(argv[i], "-O2") == 0 ||
                   strcmp(argv[i], "-O3") == 0 || strcmp(argv[i], "-Ofast") == 0) {
            opt_vectorize = !portable_cpu;
        } else if (strncmp(argv[i], "--cpu=", 6) == 0) {
            const char *cpu = argv[i] + 6;
            if (!strcmp(cpu, "portable")) {
                opt_vectorize = false;
                portable_cpu = true;
            } else if (!strcmp(cpu, "native") || !strcmp(cpu, "avx2")) {
                opt_vectorize = true;
                portable_cpu = false;
            }
            else {
                fprintf(stderr, "Error: unknown CPU target '%s' (expected native, avx2, or portable)\n", cpu);
                return 1;
            }
        } else if (strcmp(argv[i], "--warmup") == 0) {
            if (strcmp(command, "bench") != 0) {
                fprintf(stderr, "Error: --warmup is only valid with 'cobra bench'\n");
                return 1;
            }
            if (i + 1 >= argc || !parse_benchmark_count(argv[++i], &bench_warmups, "--warmup")) return 1;
        } else if (strcmp(argv[i], "--runs") == 0) {
            if (strcmp(command, "bench") != 0) {
                fprintf(stderr, "Error: --runs is only valid with 'cobra bench'\n");
                return 1;
            }
            if (i + 1 >= argc || !parse_benchmark_count(argv[++i], &bench_runs, "--runs")) return 1;
        } else if (strcmp(argv[i], "--target=win64") == 0) {
            target = TARGET_WIN64_X86_64;
        } else if (strcmp(argv[i], "--target=arm64") == 0 || strcmp(argv[i], "--target=apple") == 0) {
            target = TARGET_MACOS_ARM64;
        } else if (strcmp(argv[i], "--target=wasm") == 0 || strcmp(argv[i], "--target=wasm32") == 0) {
            target = TARGET_WASM32;
        } else if (strcmp(argv[i], "--backend=native") == 0) {
            use_isolated_backend = true;
            use_object_emitter = false;
        } else if (strcmp(argv[i], "--backend=native-object") == 0) {
            use_isolated_backend = true;
            use_object_emitter = true;
        } else if (strcmp(argv[i], "--backend=direct") == 0) {
            use_isolated_backend = false;
            use_object_emitter = false;
        }
    }

    if (use_isolated_backend && target != TARGET_LINUX_X86_64) {
        fprintf(stderr, "Error: --backend=native currently supports only the Linux x86-64 target\n");
        return 1;
    }

    if (strcmp(command, "bench") == 0 && bench_runs == 0) {
        fprintf(stderr, "Error: --runs must be greater than zero\n");
        return 1;
    }

    /* -O0 and --cpu=portable keep user loops scalar; vectorization also
       requires AVX2 on the host CPU since the emitted kernel is native. */
    if (!host_supports_avx2()) opt_vectorize = false;
    codegen_set_vectorize(opt_vectorize);
    codegen_set_portable(portable_cpu);
    codegen_set_gpu_enabled(use_gpu);

    CobraProjectConfig project_config;
    if (!discover_project_manifest(source_path, &project_config)) return 1;

    CobraSourceBuffer source_buffer = {0};
    char *std_source = read_library_file("std.cb");
    char *nn_source = read_library_file("nn.cb");
    char *fs_source = read_library_file("fs.cb");
    char *time_source = read_library_file("time.cb");
    char *mem_source = read_library_file("mem.cb");
    char *cpu_source = read_library_file("cpu.cb");
    char *net_source = read_library_file("net.cb");
    char *http_source = read_library_file("http.cb");
    if (std_source) {
        if (!source_buffer_append(&source_buffer, std_source, "lib/std.cb")) {
            fprintf(stderr, "Error: source buffer exhausted while loading lib/std.cb or source segment table is full\n");
            free(std_source);
            free(source_buffer.data);
            return 1;
        }
        free(std_source);
    }
    if (nn_source) {
        if (!source_buffer_append(&source_buffer, nn_source, "lib/nn.cb")) {
            fprintf(stderr, "Error: source buffer exhausted while loading lib/nn.cb or source segment table is full\n");
            free(nn_source);
            free(source_buffer.data);
            return 1;
        }
        free(nn_source);
    }
    if (fs_source) {
        if (!source_buffer_append(&source_buffer, fs_source, "lib/fs.cb")) {
            fprintf(stderr, "Error: source buffer exhausted while loading lib/fs.cb or source segment table is full\n");
            free(fs_source);
            free(source_buffer.data);
            return 1;
        }
        free(fs_source);
    }
    if (time_source) {
        if (!source_buffer_append(&source_buffer, time_source, "lib/time.cb")) {
            fprintf(stderr, "Error: source buffer exhausted while loading lib/time.cb or source segment table is full\n");
            free(time_source);
            free(source_buffer.data);
            return 1;
        }
        free(time_source);
    }
    if (mem_source) {
        if (!source_buffer_append(&source_buffer, mem_source, "lib/mem.cb")) {
            fprintf(stderr, "Error: source buffer exhausted while loading lib/mem.cb or source segment table is full\n");
            free(mem_source);
            free(source_buffer.data);
            return 1;
        }
        free(mem_source);
    }
    if (cpu_source) {
        if (!source_buffer_append(&source_buffer, cpu_source, "lib/cpu.cb")) {
            fprintf(stderr, "Error: source buffer exhausted while loading lib/cpu.cb or source segment table is full\n");
            free(cpu_source);
            free(source_buffer.data);
            return 1;
        }
        free(cpu_source);
    }
    if (net_source) {
        if (!source_buffer_append(&source_buffer, net_source, "lib/net.cb")) {
            fprintf(stderr, "Error: source buffer exhausted while loading lib/net.cb or source segment table is full\n");
            free(net_source);
            free(source_buffer.data);
            return 1;
        }
        free(net_source);
    }
    if (http_source) {
        if (!source_buffer_append(&source_buffer, http_source, "lib/http.cb")) {
            fprintf(stderr, "Error: source buffer exhausted while loading lib/http.cb or source segment table is full\n");
            free(http_source);
            free(source_buffer.data);
            return 1;
        }
        free(http_source);
    }
    CobraModulePaths module_paths = {0};
    if (!load_cobra_module(source_path, &project_config, &module_paths, &source_buffer)) {
        free(source_buffer.data);
        return 1;
    }
    char *combined_source = source_buffer.data;

    Parser parser;
    parser_init(&parser, combined_source);
    ASTNode *program = parser_parse_program(&parser);
    if (!annotate_source_locations(program, &source_buffer)) {
        free(combined_source);
        ast_free(program);
        return 1;
    }
    bool program_has_compute = ast_contains_compute(program);
    bool program_has_parallel = ast_contains_parallel(program);
    bool program_has_tensor_kernel = ast_contains_tensor_kernel(program);
    bool program_has_collections = ast_contains_collections(program);
    if (!host_supports_avx2()) setenv("COBRA_DISABLE_AVX2", "1", 1);

    if (strcmp(command, "fmt") == 0) {
        printf("[fmt] Formatted source file '%s'\n", source_path);
        free(combined_source);
        ast_free(program);
        return 0;
    }

    /* `check` stops after the same module composition and IR validation used by
       native builds. It deliberately does not emit assembly, link, or execute. */
    if (strcmp(command, "check") == 0) {
        CobraIR check_ir;
        if (!cobra_ir_build(program, &check_ir)) {
            free(combined_source);
            ast_free(program);
            return 1;
        }
        printf("[check] source and composed modules are valid: %s\n", source_path);
        free(combined_source);
        ast_free(program);
        return 0;
    }

    /* All other native compilation paths use the same ownership/type validation as tests.
       The isolated backend (--backend=native*) skips this: it runs its own complete,
       independent verification (bir_build_program's HIR/SSA checks, then mir_verify)
       over the reparsed user module, and its ownership/type rules are not always
       identical to the legacy validator's (e.g. freeing an owned slice received as a
       function parameter is valid there but rejected here), so gating on this pass
       would reject isolated-backend programs the isolated backend can legitimately
       compile and verify on its own. */
    if (strcmp(command, "test") != 0 && !use_isolated_backend) {
        CobraIR compile_ir;
        if (!cobra_ir_build(program, &compile_ir)) {
            free(combined_source);
            ast_free(program);
            return 1;
        }
    }

    if (strcmp(command, "test") == 0) {
        CobraIR test_ir;
        if (!cobra_ir_build(program, &test_ir)) {
            free(combined_source);
            ast_free(program);
            return 1;
        }
        if (!native_test_cpu_supports_compute(program, "[test] @compute tests")) {
            free(combined_source);
            ast_free(program);
            return 1;
        }
        if (program_has_tensor_kernel && (!host_supports_avx2() || !host_supports_fma())) {
            fprintf(stderr, "[test] f32 tensor intrinsics require AVX2 and FMA support on the host CPU\n");
            free(combined_source);
            ast_free(program);
            return 1;
        }
        printf("[test] Cobra Native Test Runner (Linux x86_64)\n");
        printf("[test] Compiling test functions for the host CPU...\n");
        bool passed = run_native_tests(program, source_path);
        free(combined_source);
        ast_free(program);
        return passed ? 0 : 1;
    }

    if (strcmp(command, "bench") == 0) {
        if (target != TARGET_LINUX_X86_64) {
            fprintf(stderr, "[bench] benchmarking currently requires the Linux x86_64 native target\n");
            free(combined_source);
            ast_free(program);
            return 1;
        }
        if (program_has_tensor_kernel && (!host_supports_avx2() || !host_supports_fma())) {
            fprintf(stderr, "[bench] f32 tensor workloads require AVX2 and FMA support on the host CPU\n");
            free(combined_source);
            ast_free(program);
            return 1;
        }
        bool passed = run_benchmark(program, source_path, program_has_parallel,
                                    bench_warmups, bench_runs);
        free(combined_source);
        ast_free(program);
        return passed ? 0 : 1;
    }

    char asm_path[256];
    if (use_object_emitter) {
        snprintf(asm_path, sizeof(asm_path), "%s.o", output_binary);
    } else if (strstr(output_binary, ".s") || strstr(output_binary, ".wat")) {
        snprintf(asm_path, sizeof(asm_path), "%s", output_binary);
    } else {
        snprintf(asm_path, sizeof(asm_path), "%s.s", output_binary);
    }

    if (target == TARGET_LINUX_X86_64 &&
        program_has_compute && !host_supports_avx2()) {
        fprintf(stderr, "[codegen] @compute requires AVX2 support on the host CPU\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }

    if (program_has_tensor_kernel && portable_cpu) {
        fprintf(stderr, "[codegen] --cpu=portable does not support f32 tensor intrinsics; use the native AVX2 target\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }

    if (program_has_tensor_kernel && target != TARGET_LINUX_X86_64) {
        fprintf(stderr, "[codegen] f32 tensor intrinsics currently require the Linux x86_64 backend\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }

    if (program_has_tensor_kernel && target == TARGET_LINUX_X86_64 &&
        (!host_supports_avx2() || !host_supports_fma())) {
        fprintf(stderr, "[codegen] f32 tensor intrinsics require AVX2 and FMA support on the host CPU\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }

    if (use_isolated_backend) {
        /* Give the isolated backend the same prelude the direct backend gets,
           so stdlib-using programs have a chance; unsupported constructs are
           still rejected below with a diagnostic instead of silently
           falling back. */
        CobraSourceBuffer isolated_buffer = {0};
        char *isolated_std_source = read_library_file("std.cb");
        char *isolated_mem_source = read_library_file("mem.cb");
        if (isolated_std_source) {
            source_buffer_append(&isolated_buffer, isolated_std_source, "lib/std.cb");
            free(isolated_std_source);
        }
        if (isolated_mem_source) {
            source_buffer_append(&isolated_buffer, isolated_mem_source, "lib/mem.cb");
            free(isolated_mem_source);
        }
        CobraModulePaths isolated_module_paths = {0};
        if (!load_cobra_module(source_path, &project_config, &isolated_module_paths,
                               &isolated_buffer)) {
            fprintf(stderr, "Error: --backend=native could not load '%s'\n", source_path);
            free(isolated_buffer.data);
            free(combined_source);
            ast_free(program);
            return 1;
        }
        Parser isolated_parser;
        parser_init(&isolated_parser, isolated_buffer.data);
        ASTNode *isolated_program = parser_parse_program(&isolated_parser);
        if (!annotate_source_locations(isolated_program, &isolated_buffer)) {
            fprintf(stderr, "Error: --backend=native could not parse '%s'\n", source_path);
            free(isolated_buffer.data);
            ast_free(isolated_program);
            free(combined_source);
            ast_free(program);
            return 1;
        }
        char bir_err[512] = {0};
        bool isolated_ok = use_object_emitter
            ? bir_backend_compile_program_object(isolated_program, source_path, asm_path,
                                                 bir_err, sizeof(bir_err))
            : bir_backend_compile_program(isolated_program, source_path, asm_path,
                                          bir_err, sizeof(bir_err));
        free(isolated_buffer.data);
        ast_free(isolated_program);
        if (!isolated_ok) {
            fprintf(stderr, "Error: --backend=%s code generation failed: %s\n",
                    use_object_emitter ? "native-object" : "native",
                    bir_err[0] ? bir_err : "unknown error");
            free(combined_source);
            ast_free(program);
            return 1;
        }
    } else if (!codegen_generate_assembly(program, asm_path, target)) {
        fprintf(stderr, "Error: Code generation failed\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }

    printf("[codegen] %s generated: %s (%s backend)\n",
           use_object_emitter ? "Object" : "Assembly", asm_path,
           use_object_emitter ? "native-object" : use_isolated_backend ? "native" : "direct");

    if (strcmp(command, "emit-asm") == 0) {
        free(combined_source);
        ast_free(program);
        return 0;
    }

    /* The isolated backend's list[T] buffer opcodes call libc malloc/free/
       memcpy directly and never need runtime/cobra_collections.c, but its
       dict[K]V opcodes do call into it (cobra_dict_*). Link it in whenever
       it can be found regardless of backend, but only hard-require it for
       the direct backend: an isolated-backend program that doesn't actually
       use dict[K]V would otherwise fail to link outside the Cobra source
       tree for no reason, and one that does will get a clear "undefined
       reference to cobra_dict_*" linker error instead of a silent
       miscompile if the runtime truly can't be found. */
    const char *runtime = (program_has_parallel && target == TARGET_LINUX_X86_64) ? parallel_runtime_path() : NULL;
    const char *collections_runtime = program_has_collections ? collections_runtime_path() : NULL;
    bool program_has_gpu_kernel = ast_has_gpu_kernel(program);
    if (program_has_gpu_kernel && !use_gpu) {
        fprintf(stderr, "[codegen] program defines an @gpu kernel; --no-gpu is incompatible with it\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }
    bool program_has_gpu = (use_gpu && ast_contains_gpu(program)) || program_has_gpu_kernel;
    const char *gpu_runtime = program_has_gpu ? gpu_runtime_path() : NULL;
    const char *gpu_kernels_file = program_has_gpu_kernel ? build_gpu_kernels_file(program, output_binary) : NULL;
    if (program_has_gpu_kernel && !gpu_kernels_file) {
        free(combined_source);
        ast_free(program);
        return 1;
    }
    if (program_has_parallel && target == TARGET_LINUX_X86_64 && !runtime) {
        fprintf(stderr, "[codegen] @parallel runtime not found; set COBRA_LIB_PATH or run from the Cobra source tree\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }
    if (program_has_collections && !use_isolated_backend && !collections_runtime) {
        fprintf(stderr, "[codegen] collections runtime not found\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }
    if (program_has_gpu && !gpu_runtime) {
        fprintf(stderr, "[codegen] gpu runtime not found; set COBRA_LIB_PATH or run from the Cobra source tree\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }
    bool program_has_precision = ast_contains_precision(program);
    const char *precision_runtime = program_has_precision ? precision_runtime_path() : NULL;
    if (program_has_precision && !precision_runtime) {
        fprintf(stderr, "[codegen] precision runtime not found; set COBRA_LIB_PATH or run from the Cobra source tree\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }
    const char *build_argv[300];
    int build_argc = 0;
    build_argv[build_argc++] = "gcc";
    build_argv[build_argc++] = "-no-pie";
    build_argv[build_argc++] = asm_path;
    if (runtime) {
        build_argv[build_argc++] = runtime;
        build_argv[build_argc++] = "-pthread";
    }
    if (collections_runtime) build_argv[build_argc++] = collections_runtime;
    if (gpu_runtime) {
        build_argv[build_argc++] = gpu_runtime;
        build_argv[build_argc++] = "-ldl";
    }
    if (precision_runtime) build_argv[build_argc++] = precision_runtime;
    if (gpu_kernels_file) build_argv[build_argc++] = gpu_kernels_file;
    const char *stackguard_runtime = target == TARGET_LINUX_X86_64 ? stackguard_runtime_path() : NULL;
    if (stackguard_runtime) build_argv[build_argc++] = stackguard_runtime;
    build_argc = append_import_libraries(program, build_argv, build_argc, 300);
    if (build_argc < 0) {
        free(combined_source);
        ast_free(program);
        return 1;
    }
    build_argv[build_argc++] = "-lm";
    build_argv[build_argc++] = "-o";
    build_argv[build_argc++] = output_binary;
    build_argv[build_argc] = NULL;
    int res = run_process(build_argv);

    if (!process_succeeded(res)) {
        fprintf(stderr, "Error: Linking failed with gcc\n");
        free(combined_source);
        ast_free(program);
        return 1;
    }

    printf("[build] Binary generated: ./%s\n", output_binary);

    if (strcmp(command, "run") == 0) {
        printf("[run] Executing binary ./%s\n", output_binary);
        char run_path[512];
        int program_code = 1;
        if (make_local_exec_path(output_binary, run_path, sizeof(run_path))) {
            const char *run_argv[] = {run_path, NULL};
            int exit_code = run_process(run_argv);
            program_code = WIFEXITED(exit_code) ? WEXITSTATUS(exit_code) : 1;
        } else {
            fprintf(stderr, "[run] executable path is too long\n");
        }
        printf("Process exited with code: %d\n", program_code);
        free(combined_source);
        ast_free(program);
        return program_code;
    }

    free(combined_source);
    ast_free(program);
    return 0;
}
