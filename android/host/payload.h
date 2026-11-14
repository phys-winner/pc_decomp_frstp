#pragma once
#include "cpu64.h"

typedef void (*frostpunk_payload_fn)(CPU *c);
/* NULL outside the payload; a target inside it must match original static C. */
frostpunk_payload_fn frostpunk_payload_lookup(uint64_t target);
/* The normal wrapper resolves IAT dynamically; this also supports its explicit
 * payload sentinel namespace if a host loader installs these exact imports. */
int frostpunk_payload_import(CPU *c, uint64_t target);
