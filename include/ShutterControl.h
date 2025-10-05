#include "lwip/ip_addr.h"

#define SHUTTER_VENTILATION 17

enum ShutterStatus {
    SHUTTER_STATUS_UNKNOWN = 0,
    SHUTTER_STATUS_CLOSED,
    SHUTTER_STATUS_CLOSING,
    SHUTTER_STATUS_OPEN,
    SHUTTER_STATUS_OPENING,
    SHUTTER_STATUS_STOPPED
};

void OpenShutter(ip4_addr_t shutter_ip);
void CloseShutter(ip4_addr_t shutter_ip);
void StopShutter(ip4_addr_t shutter_ip);
enum ShutterStatus GetShutterStatus(ip4_addr_t shutter_ip);
void SetShutterPosition(ip4_addr_t shutter_ip, int position);