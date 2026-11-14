/* 渲染分辨率（1080p / 1440p）在启动时写进 main：不需要重新改写镜像，也不需要重打 app。
   Render resolution (1080p / 1440p) written into main at startup, so changing it needs neither a rewrite nor a rebuild. */
#pragma once
#include <stddef.h>

/* 从映像里读 GNU build-id（32 个十六进制字符 + 终止符），找不到给空串。 | The image's GNU build-id, or "" when absent. */
void runtime_render_res_build_id(const void *image, size_t size, char out[33]);

/* 按设置把 15 处尺寸立即数写成 1080p 或 1440p；build id 不是登记过的那份就不写，逐条校验失败的那一条跳过。
   Writes the 15 size immediates for the configured size; refuses an unregistered build and skips any site whose word is
   not one of the two known ones. */
void runtime_render_res_apply(void *main_base, const char *build_id);
