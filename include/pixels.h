#pragma once

#include <wayland-client.h>
#include <pixman.h>
#include <stdbool.h>

bool wl_shm_to_pixman_fmt(pixman_format_code_t* dst, enum wl_shm_format src);
