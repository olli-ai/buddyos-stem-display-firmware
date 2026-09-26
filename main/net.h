#pragma once
#include <stdbool.h>
/** Bring up Wi-Fi station, then MQTT. Both are asynchronous; mem_report() is called
 *  at each milestone, which is the point of having them here at all. */
void net_start(void);
