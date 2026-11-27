#pragma once
#include "ipc.h"
/* Return zero when experimental networking is disabled; preserve offline path. */
int mng_lan_bsd_dispatch(const IpcRequest *request, IpcResponse *response);
