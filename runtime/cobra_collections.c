#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

char *cobra_str_copy(const char *s);

static void cobra_list_grow(void **data, size_t *capacity, size_t length, size_t element_size) {
    if (length == SIZE_MAX || (element_size != 0 && length + 1 > SIZE_MAX / element_size)) abort();
    size_t next = *capacity ? *capacity : 8;
    if (next > SIZE_MAX / 2) next = SIZE_MAX / element_size;
    else next *= 2;
    if (next < length + 1) next = length + 1;
    if (next == 0 || next > SIZE_MAX / element_size) abort();
    void *grown = realloc(*data, next * element_size);
    if (!grown) abort();
    *data = grown;
    *capacity = next;
}

void cobra_list_append_i64(void **data, size_t *length, size_t *capacity, int64_t value) {
    if (*length == *capacity) cobra_list_grow(data, capacity, *length, sizeof(int64_t));
    ((int64_t *)*data)[(*length)++] = value;
}

void cobra_list_append_f32(void **data, size_t *length, size_t *capacity, float value) {
    if (*length == *capacity) cobra_list_grow(data, capacity, *length, sizeof(float));
    ((float *)*data)[(*length)++] = value;
}

void cobra_list_free(void **data, size_t *length, size_t *capacity) {
    free(*data);
    *data = NULL;
    *length = 0;
    *capacity = 0;
}

/* Owned list[string] contract: every element slot holds an owned string the
   list created (append copies, split allocates). Freeing a string list
   releases each element before the block itself. */
static void cobra_list_push_string(void **data, size_t *length, size_t *capacity, char *value) {
    if (*length == *capacity) cobra_list_grow(data, capacity, *length, sizeof(char *));
    ((char **)*data)[(*length)++] = value;
}

void cobra_list_append_string(void **data, size_t *length, size_t *capacity, const char *value) {
    cobra_list_push_string(data, length, capacity, cobra_str_copy(value));
}

void cobra_list_free_strings(void **data, size_t *length, size_t *capacity) {
    for (size_t i = 0; i < *length; i++) free(((char **)*data)[i]);
    free(*data);
    *data = NULL;
    *length = 0;
    *capacity = 0;
}

/* In-place reversal over element_size-wide slots; works for every element
   layout (scalar, float, owned string pointers, struct block pointers). */
void cobra_list_reverse(void *data, size_t length, size_t element_size) {
    if (!data || element_size == 0) return;
    unsigned char *base = (unsigned char *)data;
    for (size_t lo = 0, hi = length - 1; lo < hi; lo++, hi--) {
        for (size_t b = 0; b < element_size; b++) {
            unsigned char t = base[lo * element_size + b];
            base[lo * element_size + b] = base[hi * element_size + b];
            base[hi * element_size + b] = t;
        }
    }
}

/* clear drops the logical contents but keeps the allocated block; string
   lists free every owned element first. */
void cobra_list_clear(void **data, size_t *length, size_t *capacity) {
    (void)data;
    (void)capacity;
    *length = 0;
}

void cobra_list_clear_strings(void **data, size_t *length, size_t *capacity) {
    for (size_t i = 0; i < *length; i++) free(((char **)*data)[i]);
    *length = 0;
    (void)capacity;
}

static int cobra_list_cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int cobra_list_cmp_f32(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static int cobra_list_cmp_string(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void cobra_list_sort_i64(void *data, size_t length) {
    if (data && length > 1) qsort(data, length, sizeof(int64_t), cobra_list_cmp_i64);
}

void cobra_list_sort_f32(void *data, size_t length) {
    if (data && length > 1) qsort(data, length, sizeof(float), cobra_list_cmp_f32);
}

void cobra_list_sort_strings(char **data, size_t length) {
    if (data && length > 1) qsort(data, length, sizeof(char *), cobra_list_cmp_string);
}

size_t cobra_list_count_i64(void *data, size_t length, int64_t value) {
    size_t count = 0;
    for (size_t i = 0; i < length; i++) if (((int64_t *)data)[i] == value) count++;
    return count;
}

size_t cobra_list_count_f32(void *data, size_t length, float value) {
    size_t count = 0;
    for (size_t i = 0; i < length; i++) if (((float *)data)[i] == value) count++;
    return count;
}

size_t cobra_list_count_strings(char **data, size_t length, const char *value) {
    size_t count = 0;
    for (size_t i = 0; i < length; i++) if (data[i] && strcmp(data[i], value) == 0) count++;
    return count;
}

/* index returns the first position of value, or -1 when absent (the
   collections runtime uses the same sentinel convention as dict get/pop). */
int64_t cobra_list_index_i64(void *data, size_t length, int64_t value) {
    for (size_t i = 0; i < length; i++) if (((int64_t *)data)[i] == value) return (int64_t)i;
    return -1;
}

int64_t cobra_list_index_f32(void *data, size_t length, float value) {
    for (size_t i = 0; i < length; i++) if (((float *)data)[i] == value) return (int64_t)i;
    return -1;
}

int64_t cobra_list_index_strings(char **data, size_t length, const char *value) {
    for (size_t i = 0; i < length; i++) if (data[i] && strcmp(data[i], value) == 0) return (int64_t)i;
    return -1;
}

/* extend copies every element of src into dst; string elements are copied so
   the destination owns each one, matching append_string's contract. */
void cobra_list_extend_i64(void **data, size_t *length, size_t *capacity,
                           void *src, size_t src_length) {
    for (size_t i = 0; i < src_length; i++)
        cobra_list_append_i64(data, length, capacity, ((int64_t *)src)[i]);
}

void cobra_list_extend_f32(void **data, size_t *length, size_t *capacity,
                           void *src, size_t src_length) {
    for (size_t i = 0; i < src_length; i++)
        cobra_list_append_f32(data, length, capacity, ((float *)src)[i]);
}

void cobra_list_extend_strings(void **data, size_t *length, size_t *capacity,
                               char **src, size_t src_length) {
    for (size_t i = 0; i < src_length; i++)
        cobra_list_push_string(data, length, capacity, cobra_str_copy(src[i]));
}

static void cobra_list_shift_right(void **data, size_t *length, size_t *capacity,
                                   size_t index, size_t element_size) {
    if (*length == *capacity) cobra_list_grow(data, capacity, *length, element_size);
    unsigned char *base = (unsigned char *)*data;
    memmove(base + (index + 1) * element_size, base + index * element_size,
            (*length - index) * element_size);
    (*length)++;
}

void cobra_list_insert_i64(void **data, size_t *length, size_t *capacity,
                           size_t index, int64_t value) {
    if (index > *length) index = *length;
    cobra_list_shift_right(data, length, capacity, index, sizeof(int64_t));
    ((int64_t *)*data)[index] = value;
}

void cobra_list_insert_f32(void **data, size_t *length, size_t *capacity,
                           size_t index, float value) {
    if (index > *length) index = *length;
    cobra_list_shift_right(data, length, capacity, index, sizeof(float));
    ((float *)*data)[index] = value;
}

void cobra_list_insert_string(void **data, size_t *length, size_t *capacity,
                              size_t index, const char *value) {
    if (index > *length) index = *length;
    cobra_list_shift_right(data, length, capacity, index, sizeof(char *));
    ((char **)*data)[index] = cobra_str_copy(value);
}

static void cobra_list_shift_left(void **data, size_t *length, size_t index,
                                  size_t element_size) {
    unsigned char *base = (unsigned char *)*data;
    memmove(base + index * element_size, base + (index + 1) * element_size,
            (*length - index - 1) * element_size);
    (*length)--;
}

/* remove_value drops the first occurrence and returns 1, or 0 when absent
   (Python raises ValueError there; the collections runtime prefers a
   testable sentinel like index's -1). */
int64_t cobra_list_remove_value_i64(void **data, size_t *length, int64_t value) {
    for (size_t i = 0; i < *length; i++) {
        if (((int64_t *)*data)[i] == value) {
            cobra_list_shift_left(data, length, i, sizeof(int64_t));
            return 1;
        }
    }
    return 0;
}

int64_t cobra_list_remove_value_f32(void **data, size_t *length, float value) {
    for (size_t i = 0; i < *length; i++) {
        if (((float *)*data)[i] == value) {
            cobra_list_shift_left(data, length, i, sizeof(float));
            return 1;
        }
    }
    return 0;
}

int64_t cobra_list_remove_value_strings(void **data, size_t *length, const char *value) {
    for (size_t i = 0; i < *length; i++) {
        if (((char **)*data)[i] && strcmp(((char **)*data)[i], value) == 0) {
            free(((char **)*data)[i]);
            cobra_list_shift_left(data, length, i, sizeof(char *));
            return 1;
        }
    }
    return 0;
}

/* Content equality over list descriptors: lengths must match and every
   element must compare equal (strings by value, not by pointer). */
int64_t cobra_list_equal_i64(void *a, size_t a_length, void *b, size_t b_length) {
    if (a_length != b_length) return 0;
    if (a_length == 0) return 1;
    return !memcmp(a, b, a_length * sizeof(int64_t));
}

int64_t cobra_list_equal_f32(void *a, size_t a_length, void *b, size_t b_length) {
    if (a_length != b_length) return 0;
    for (size_t i = 0; i < a_length; i++) {
        uint32_t x, y;
        memcpy(&x, &((float *)a)[i], sizeof(x));
        memcpy(&y, &((float *)b)[i], sizeof(y));
        if (x != y) return 0;
    }
    return 1;
}

int64_t cobra_list_equal_strings(char **a, size_t a_length, char **b, size_t b_length) {
    if (a_length != b_length) return 0;
    for (size_t i = 0; i < a_length; i++) {
        if ((a[i] == NULL) != (b[i] == NULL)) return 0;
        if (a[i] && strcmp(a[i], b[i]) != 0) return 0;
    }
    return 1;
}

/* Python-style slice over a list: negative bounds count from the end and
   out-of-range bounds clamp. Returns an owned copy of the selected window
   (string elements are duplicated) so the slice shares no element storage
   with the source. */
static size_t cobra_list_clamp(size_t length, int64_t bound) {
    if (bound < 0) {
        int64_t from_end = (int64_t)length + bound;
        if (from_end < 0) return 0;
        return (size_t)from_end;
    }
    return (size_t)bound > length ? length : (size_t)bound;
}

/* The list slice descriptor is the same {data, length, capacity} triple the
   direct emitter's sret buffers use for split and list-returning calls. */
typedef struct {
    void *data;
    size_t length;
    size_t capacity;
} CobraListSlice;

void cobra_list_slice_i64(void *data, size_t length, int64_t start, int64_t end,
                          CobraListSlice *out) {
    size_t lo = cobra_list_clamp(length, start), hi = cobra_list_clamp(length, end);
    out->data = NULL;
    out->length = 0;
    out->capacity = 0;
    if (hi <= lo) return;
    size_t n = hi - lo;
    out->data = malloc(n * sizeof(int64_t));
    if (!out->data) abort();
    memcpy(out->data, (int64_t *)data + lo, n * sizeof(int64_t));
    out->length = n;
    out->capacity = n;
}

void cobra_list_slice_f32(void *data, size_t length, int64_t start, int64_t end,
                          CobraListSlice *out) {
    size_t lo = cobra_list_clamp(length, start), hi = cobra_list_clamp(length, end);
    out->data = NULL;
    out->length = 0;
    out->capacity = 0;
    if (hi <= lo) return;
    size_t n = hi - lo;
    out->data = malloc(n * sizeof(float));
    if (!out->data) abort();
    memcpy(out->data, (float *)data + lo, n * sizeof(float));
    out->length = n;
    out->capacity = n;
}

void cobra_list_slice_strings(char **data, size_t length, int64_t start, int64_t end,
                              CobraListSlice *out) {
    size_t lo = cobra_list_clamp(length, start), hi = cobra_list_clamp(length, end);
    out->data = NULL;
    out->length = 0;
    out->capacity = 0;
    if (hi <= lo) return;
    size_t n = hi - lo;
    char **copy = malloc(n * sizeof(char *));
    if (!copy) abort();
    for (size_t i = 0; i < n; i++) copy[i] = cobra_str_copy(data[lo + i]);
    out->data = copy;
    out->length = n;
    out->capacity = n;
}

/* Splits s on every occurrence of sep into owned pieces. An empty separator
   yields no pieces, matching a defensive reading of the Python contract. */
void cobra_str_split(const char *s, const char *sep, void **data, size_t *length, size_t *capacity) {
    *data = NULL;
    *length = 0;
    *capacity = 0;
    if (!s || !sep || *sep == '\0') return;
    size_t sep_len = strlen(sep);
    const char *scan = s;
    for (;;) {
        const char *hit = strstr(scan, sep);
        size_t piece_len = hit ? (size_t)(hit - scan) : strlen(scan);
        char *piece = malloc(piece_len + 1);
        if (!piece) abort();
        memcpy(piece, scan, piece_len);
        piece[piece_len] = '\0';
        cobra_list_push_string(data, length, capacity, piece);
        if (!hit) break;
        scan = hit + sep_len;
    }
}

/* Joins count owned strings with sep into a fresh owned string. */
char *cobra_str_join(const char *sep, const char **items, size_t count) {
    if (!sep) sep = "";
    size_t sep_len = strlen(sep);
    size_t total = 1;
    for (size_t i = 0; i < count; i++) {
        if (items[i]) total += strlen(items[i]);
        if (i + 1 < count) total += sep_len;
    }
    char *out = malloc(total);
    if (!out) abort();
    char *w = out;
    for (size_t i = 0; i < count; i++) {
        if (items[i]) {
            size_t n = strlen(items[i]);
            memcpy(w, items[i], n);
            w += n;
        }
        if (i + 1 < count) {
            memcpy(w, sep, sep_len);
            w += sep_len;
        }
    }
    *w = '\0';
    return out;
}

typedef struct {
    char *key;
    int64_t value;
    unsigned char used;
} CobraDictEntry;

typedef struct {
    size_t capacity;
    size_t length;
    CobraDictEntry entries[];
} CobraDict;

static uint64_t cobra_hash_string(const char *key) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static CobraDict *cobra_dict_new(size_t capacity) {
    if (capacity < 8) capacity = 8;
    if (capacity > (SIZE_MAX - sizeof(CobraDict)) / sizeof(CobraDictEntry)) abort();
    CobraDict *dict = calloc(1, sizeof(*dict) + capacity * sizeof(dict->entries[0]));
    if (!dict) abort();
    dict->capacity = capacity;
    return dict;
}

static void cobra_dict_rehash(CobraDict **owner) {
    CobraDict *old = *owner;
    if (old->capacity > SIZE_MAX / 2) abort();
    CobraDict *next = cobra_dict_new(old->capacity * 2);
    for (size_t i = 0; i < old->capacity; i++) {
        CobraDictEntry *entry = &old->entries[i];
        if (!entry->used) continue;
        size_t slot = (size_t)(cobra_hash_string(entry->key) % next->capacity);
        while (next->entries[slot].used) slot = (slot + 1) % next->capacity;
        next->entries[slot] = *entry;
        next->length++;
    }
    free(old);
    *owner = next;
}

static CobraDictEntry *cobra_dict_find(CobraDict *dict, const char *key, int create) {
    if (!dict) return NULL;
    if (create && (dict->length == SIZE_MAX || dict->capacity > SIZE_MAX / 10 ||
                    (dict->length + 1) * 10 >= dict->capacity * 7)) return NULL;
    size_t slot = (size_t)(cobra_hash_string(key) % dict->capacity);
    for (;;) {
        CobraDictEntry *entry = &dict->entries[slot];
        if (!entry->used) {
            if (!create) return NULL;
            entry->key = strdup(key);
            if (!entry->key) abort();
            entry->used = 1;
            dict->length++;
            return entry;
        }
        if (strcmp(entry->key, key) == 0) return entry;
        slot = (slot + 1) % dict->capacity;
    }
}

void cobra_dict_set_i64(void **owner, const char *key, int64_t value) {
    if (!*owner) *owner = cobra_dict_new(8);
    CobraDict *dict = *owner;
    CobraDictEntry *entry = cobra_dict_find(dict, key, 1);
    if (!entry) {
        cobra_dict_rehash(&dict);
        *owner = dict;
        entry = cobra_dict_find(dict, key, 1);
    }
    entry->value = value;
}

int64_t cobra_dict_get_i64(void *owner, const char *key, int64_t fallback) {
    CobraDict *dict = owner;
    CobraDictEntry *entry = cobra_dict_find(dict, key, 0);
    return entry ? entry->value : fallback;
}

int64_t cobra_dict_has(void *owner, const char *key) {
    CobraDict *dict = owner;
    return cobra_dict_find(dict, key, 0) != NULL;
}

int64_t cobra_dict_delete(void **owner, const char *key) {
    if (!owner || !*owner) return 0;
    CobraDict *dict = *owner;
    CobraDictEntry *entry = cobra_dict_find(dict, key, 0);
    if (!entry) return 0;
    free(entry->key);
    entry->key = NULL;
    entry->used = 0;
    dict->length--;
    return 1;
}

int64_t cobra_dict_pop(void **owner, const char *key, int64_t fallback) {
    if (!owner || !*owner) return fallback;
    CobraDict *dict = *owner;
    CobraDictEntry *entry = cobra_dict_find(dict, key, 0);
    if (!entry) return fallback;
    int64_t value = entry->value;
    free(entry->key);
    entry->key = NULL;
    entry->used = 0;
    dict->length--;
    return value;
}

size_t cobra_dict_len(void *owner) {
    CobraDict *dict = owner;
    return dict ? dict->length : 0;
}

/* Struct-valued dicts (dict[string]Struct) store a heap pointer to a private
   copy of the struct as the entry's already-8-byte int64 value slot (see
   emit_dict_set_key's struct case in codegen.c) - the runtime itself never
   needs to know that, it just carries the bits. Destroying such a dict has
   to free each live entry's struct copy (and that copy's own owned fields)
   before the dict container goes away, but the struct field layout is only
   known to codegen, not this runtime. These two accessors expose just enough
   of CobraDict's layout (entry array base and live capacity) for codegen to
   walk entries itself; entry stride/offsets are a fixed ABI documented next
   to CobraDictEntry above (24 bytes; value at +8, used flag at +16). */
size_t cobra_dict_capacity(void *owner) {
    CobraDict *dict = owner;
    return dict ? dict->capacity : 0;
}

void *cobra_dict_raw_entries(void *owner) {
    CobraDict *dict = owner;
    return dict ? dict->entries : NULL;
}

void cobra_dict_free(void **owner, size_t *length) {
    CobraDict *dict = *owner;
    if (dict) {
        for (size_t i = 0; i < dict->capacity; i++) free(dict->entries[i].key);
        free(dict);
    }
    *owner = NULL;
    if (length) *length = 0;
}

/* String slices back s[start:end]; negative bounds count from the end and
   out-of-range bounds clamp, so the result always fits within the source. */
char *cobra_str_substring(const char *s, int64_t start, int64_t end) {
    if (!s) return strdup("");
    size_t n = strlen(s);
    int64_t si = start, ei = end;
    if (si < 0) si += (int64_t)n;
    if (ei < 0) ei += (int64_t)n;
    if (si < 0) si = 0;
    if (ei < 0) ei = 0;
    if (si > (int64_t)n) si = (int64_t)n;
    if (ei > (int64_t)n) ei = (int64_t)n;
    if (ei < si) ei = si;
    size_t len = (size_t)(ei - si);
    char *out = malloc(len + 1);
    if (!out) abort();
    memcpy(out, s + si, len);
    out[len] = '\0';
    return out;
}

/* String methods: the caller owns the returned malloc'd buffer. These back
   s.upper()/s.lower()/s.strip()/s.replace(...), which codegen lowers to
   direct calls just like the collection helpers above. */
char *cobra_str_upper(const char *s) {
    if (!s) return strdup("");
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) abort();
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    out[n] = '\0';
    return out;
}

char *cobra_str_lower(const char *s) {
    if (!s) return strdup("");
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) abort();
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out[n] = '\0';
    return out;
}

/* Owned copy of a string, used by str(string) so the caller owns the
   result instead of aliasing the argument. */
char *cobra_str_copy(const char *s) {
    if (!s) return strdup("");
    return strdup(s);
}

static int cobra_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

char *cobra_str_strip(const char *s) {
    if (!s) return strdup("");
    size_t n = strlen(s);
    size_t start = 0, end = n;
    while (start < end && cobra_is_space(s[start])) start++;
    while (end > start && cobra_is_space(s[end - 1])) end--;
    char *out = malloc(end - start + 1);
    if (!out) abort();
    memcpy(out, s + start, end - start);
    out[end - start] = '\0';
    return out;
}

char *cobra_str_replace(const char *s, const char *old, const char *new) {
    if (!s || !old || !new) return s ? strdup(s) : strdup("");
    size_t old_len = strlen(old);
    if (old_len == 0) return strdup(s);
    size_t new_len = strlen(new);
    size_t count = 0;
    for (const char *p = s; (p = strstr(p, old)) != NULL; p += old_len) count++;
    size_t out_len = strlen(s) + count * (new_len > old_len ? new_len - old_len : 0);
    char *out = malloc(out_len + 1);
    if (!out) abort();
    char *w = out;
    const char *scan = s;
    const char *hit;
    while ((hit = strstr(scan, old)) != NULL) {
        size_t span = (size_t)(hit - scan);
        memcpy(w, scan, span);
        w += span;
        memcpy(w, new, new_len);
        w += new_len;
        scan = hit + old_len;
    }
    strcpy(w, scan);
    return out;
}
