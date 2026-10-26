#pragma once
#include <stdbool.h>
// Serves a technician-facing HTTP SD card file manager on port 80.
// No user/folder/file authorization; access relies on the private Wi-Fi network
// and the user's explicit File Manager ON setting. Operations are sandboxed to
// /sdcard to protect the device's own firmware and NVS.
void web_files_tick(bool enabled,bool network_up);
bool web_files_running(void);
