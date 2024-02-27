#pragma once

#include "vec_types/vec_types.h"

typedef struct {
    vec3 color;
    vec4 properties;
} Material;

Material material(vec3 color, float emission);