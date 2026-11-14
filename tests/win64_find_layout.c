/* Host-only Windows SDK ABI check; never linked into the Android runtime. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
_Static_assert(sizeof(WIN32_FIND_DATAW)==592,"Win64 find W size");
_Static_assert(sizeof(WIN32_FIND_DATAA)==320,"Win64 find A size");
_Static_assert(offsetof(WIN32_FIND_DATAW,cFileName)==44,"Win64 find filename offset");
_Static_assert(offsetof(WIN32_FIND_DATAW,cAlternateFileName)==564,"Win64 alternate filename offset");
int main(void) {
    printf("{\"WIN32_FIND_DATAW\":%zu,\"WIN32_FIND_DATAA\":%zu,\"filename_offset\":%zu,\"alternate_w_offset\":%zu,\"FindExInfoBasic\":%d,\"FindExSearchLimitToDirectories\":%d,\"FIND_FIRST_EX_CASE_SENSITIVE\":%u,\"FIND_FIRST_EX_LARGE_FETCH\":%u}\n",
        sizeof(WIN32_FIND_DATAW),sizeof(WIN32_FIND_DATAA),offsetof(WIN32_FIND_DATAW,cFileName),offsetof(WIN32_FIND_DATAW,cAlternateFileName),
        FindExInfoBasic,FindExSearchLimitToDirectories,FIND_FIRST_EX_CASE_SENSITIVE,FIND_FIRST_EX_LARGE_FETCH);
    return 0;
}
