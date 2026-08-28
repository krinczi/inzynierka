#ifndef INC_OTA_MANAGER_H_
#define INC_OTA_MANAGER_H_

#include <stddef.h>
#include <stdint.h>

#include "ota_control.h"

typedef struct
{
    char job_id[OTA_CONTROL_JOB_ID_SIZE];
    uint32_t execution_version;
    uint32_t image_version;
    int succeeded;
    int rejected;
} OtaPendingJob;

/* Loads the bootloader state without modifying Flash. */
int OtaManager_Init(void);

/*
 * Confirms a TRIAL image after the caller has established the production
 * health gate: persistent services, IWDG and a stable MQTT session.
 * Confirmation advances the anti-rollback floor.
 */
int OtaManager_ConfirmHealthy(void);

/* Resets into the bootloader only when a verified candidate is READY. */
int OtaManager_RequestInstall(void);

/*
 * Authenticates the previous image in the secondary slot and atomically
 * prepares a reverse swap. The caller must acknowledge the command before
 * rebooting into the bootloader.
 */
int OtaManager_PrepareManualRollback(uint32_t *target_version);

/* Resets an unconfirmed trial when its health deadline expires. */
void OtaManager_Process(uint32_t now_ms);

int OtaManager_GetPendingJob(OtaPendingJob *job);
int OtaManager_GetReadyInstallJob(OtaPendingJob *job);
/* Returns the authenticated image version currently installed in primary. */
uint32_t OtaManager_GetInstalledImageVersion(void);
/*
 * Persists the execution version returned by AWS for the accepted REBOOTING
 * update. The terminal report after reset must use this newer version.
 */
int OtaManager_UpdateJobExecutionVersion(uint32_t execution_version);
int OtaManager_MarkJobReported(void);
int OtaManager_IsTrial(void);

#endif /* INC_OTA_MANAGER_H_ */
