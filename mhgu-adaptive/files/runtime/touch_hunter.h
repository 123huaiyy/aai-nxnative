#pragma once
#include <stdint.h>
struct SDL_Window;
void hunter_touch_attach(struct SDL_Window *window);
void hunter_touch_set_controller_connected(int connected);
void hunter_touch_read(uint64_t *buttons, int *lx, int *ly, int *rx, int *ry);
int hunter_touch_take_menu(void);
