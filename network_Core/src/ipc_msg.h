/* ========================================================
 * ipc_msg.h — shared IPC message definitions
 * Keep in common/ folder
 * Include in BOTH app_core and net_core
 * ======================================================== */
#ifndef IPC_MSG_H
#define IPC_MSG_H

#include <stdint.h>

/* Message type — first byte of every IPC message */
#define IPC_MSG_TYPE_DATA    0x01   /* APP→NET: Sensor mfg data to advertise */
#define IPC_MSG_TYPE_TRIGGER 0x02   /* NET→APP: mobile triggered, read sensor */
#define IPC_MSG_TYPE_RESET   0x03   /* NET→APP: clear sensor data */
#define IPC_MSG_TYPE_ACK     0x04   /* NET→APP: adv done */
#define IPC_MSG_TYPE_NACK    0x05   /* APP→NET: no data available */

/* Weather Station mfg data size */
#define WS_MFG_LEN           35

/* Max IPC message = 1 type byte + 35 bytes mfg data */
#define IPC_MAX_MSG_SIZE     (1 + WS_MFG_LEN)

#endif /* IPC_MSG_H */