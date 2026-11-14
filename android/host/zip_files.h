#pragma once
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

typedef struct fp_data_file {
    int fd[2];
    uint64_t offset[2], length[2], size;
    struct stat stat;
} fp_data_file;

int fp_data_open(const char *relative, fp_data_file *file);
int fp_data_root_matches(const char *path);
void fp_data_close(fp_data_file *file);
long long fp_data_read(const fp_data_file *file, void *buffer, size_t size, uint64_t at);
int fp_data_stat(const char *relative, struct stat *st);
FILE *fp_data_stream(const fp_data_file *file);
FILE *fp_data_fopen(const char *path, const char *mode);
void *fp_data_map(const fp_data_file *file, uint64_t at, size_t size, int prot, void *requested, void **base, size_t *bytes);
int fp_data_next(size_t *cursor, char *name, size_t capacity, struct stat *st);
