#pragma once

#include <stdbool.h>

bool save_open(void);
bool save_prepare_transfer(void);
bool save_begin_transfer(void);
bool save_begin_import(bool recovery);
void save_end_transfer(void);
bool save_commit(void);
