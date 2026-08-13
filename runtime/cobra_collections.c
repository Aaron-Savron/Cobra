#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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

void cobra_dict_free(void **owner, size_t *length) {
    CobraDict *dict = *owner;
    if (dict) {
        for (size_t i = 0; i < dict->capacity; i++) free(dict->entries[i].key);
        free(dict);
    }
    *owner = NULL;
    if (length) *length = 0;
}
