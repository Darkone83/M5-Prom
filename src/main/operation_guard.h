#pragma once
#include <stdbool.h>
/*
 * Non-blocking, shared SD/EEPROM maintenance gate. Both the UI task and the
 * HTTP server task must acquire this gate for any filesystem operation.
 * Long HTTP uploads keep it until file close/verify/rename completes.
 * The UI keeps it from PREWRITE snapshot through write confirm/cancel/verify.
 * A refused acquire never waits, so pressing a key cannot stall behind a
 * slow upload, and a browser request receives HTTP 409 rather than racing.
 */
bool operation_guard_init(void);
bool operation_guard_try_acquire(void);
void operation_guard_release(void);
