#pragma once

#define vec_SHORT_NAMES
#include "vector.h"

typedef struct {
    vec3 color;
    vec4 properties;
} Material;

Material material(vec3 color, float emission);