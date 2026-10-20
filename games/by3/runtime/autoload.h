/* TOTK_AUTOLOAD=1 (desktop launcher, user test sessions): load the newest save -- the first entry of the title's load
   list, the one "继续" loads -- without the user pressing through the title. See runtime/autoload.c. */
#pragma once

void runtime_autoload_save_file_opened(const char *path);   /* fs_host.c: a host (save) file was opened */
void runtime_autoload_user_input(void);                     /* input.c: the user pressed a key or controller button */
void runtime_input_key_synthetic(unsigned code, int down);  /* input.c: a key press that is not the user's */
