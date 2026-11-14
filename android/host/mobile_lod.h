#pragma once
/* Lowest mesh level of detail (0 = full detail .. 3) the camera may select; see mobile_lod.c. */
void fp_mobile_lod_minimum(unsigned level);
/* The level actually handed to the engine for a requested one. */
unsigned fp_mobile_lod_level(unsigned requested);
