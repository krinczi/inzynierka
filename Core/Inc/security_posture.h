#ifndef INC_SECURITY_POSTURE_H_
#define INC_SECURITY_POSTURE_H_

#include <stdint.h>

typedef enum
{
    SECURITY_RDP_LEVEL_0 = 0,
    SECURITY_RDP_LEVEL_1,
    SECURITY_RDP_LEVEL_2,
    SECURITY_RDP_UNKNOWN
} SecurityRdpLevel;

typedef struct
{
    SecurityRdpLevel rdp_level;
    uint8_t debugger_attached;
    uint8_t mpu_enabled;
} SecurityPosture;

/* Configures non-executable/read-only MPU regions for persistent Flash. */
void SecurityPosture_ConfigureMpu(void);

/* Read-only inspection; this function never programs option bytes. */
SecurityPosture SecurityPosture_Evaluate(void);
void SecurityPosture_Report(void);

const char *SecurityPosture_RdpName(SecurityRdpLevel level);

#endif /* INC_SECURITY_POSTURE_H_ */
