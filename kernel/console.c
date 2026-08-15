/* cyd-os — console arbitration. See console.h. */

#include "console.h"
#include "mutex.h"

static mutex_t g_console;

void console_init(void)   { mutex_init(&g_console); }
void console_lock(void)   { mutex_lock(&g_console); }
void console_unlock(void) { mutex_unlock(&g_console); }

unsigned int console_contentions(void) { return g_console.contentions; }
int console_owner(void) { return mutex_owner(&g_console); }
