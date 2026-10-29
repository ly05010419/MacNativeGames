#pragma once
#include <stdint.h>

/* Enable before game threads start. A successful begin owns the gate until end. */
void runtime_fs_progress_enable(void);
int runtime_fs_progress_snapshot_begin(uint64_t *generation);
void runtime_fs_progress_snapshot_end(void);
int runtime_fs_progress_save_pending(void);
