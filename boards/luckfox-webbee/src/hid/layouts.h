#ifndef GHOST_HID_LAYOUTS_H
#define GHOST_HID_LAYOUTS_H

#include <stdint.h>

#define LAYOUT_MAX 1024

typedef struct {
    char    name[8];   /* "a", "A", "ENTER", "F1", ... */
    uint8_t mod;
    uint8_t key;
} layout_entry_t;

int layout_load(const char *path);
const layout_entry_t *layout_find(const char *name);
const layout_entry_t *layout_find_ch(char c);
const char *layout_name(void);

#endif
