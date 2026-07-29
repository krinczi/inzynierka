#include "ota_manager.h"

#include "debug_logger.h"
#include "main.h"
#include "ota_control.h"
#include "ota_image.h"

#include <stdio.h>
#include <string.h>

static uint8_t ota_trial_active;
static uint32_t ota_trial_deadline_ms;

static void OtaManager_LogRecord(const char *prefix,
                                 const OtaControlRecord *record)
{
    char message[176];

    (void)snprintf(message,
                   sizeof(message),
                   "OTA: %s state=%s seq=%lu confirmed=%lu primary=%lu trial=%lu",
                   prefix,
                   OtaControl_StateName(record->state),
                   (unsigned long)record->sequence,
                   (unsigned long)record->confirmed_version,
                   (unsigned long)record->primary.image_version,
                   (unsigned long)record->trial_boot_count);
    Debug_LogLine(DEBUG_SRC_SYSTEM, message);
}

int OtaManager_Init(void)
{
    OtaControlRecord record;
    OtaControlResult result = OtaControl_LoadLatest(&record);

    ota_trial_active = 0U;
    ota_trial_deadline_ms = 0U;
    if (result == OTA_CONTROL_NOT_FOUND)
    {
        OtaImageDescriptor factory_image;

        if (OtaImage_VerifySlot(OTA_PRIMARY_SLOT_ADDRESS,
                                &factory_image) != OTA_IMAGE_OK)
        {
            Debug_LogLine(DEBUG_SRC_SYSTEM,
                          "OTA: factory image authentication failed");
            return -1;
        }
        memset(&record, 0, sizeof(record));
        record.state = OTA_CONTROL_STATE_CONFIRMED;
        record.confirmed_version = factory_image.image_version;
        record.primary = factory_image;
        record.flags = OTA_CONTROL_FLAG_FIRMWARE;
        if (OtaControl_Append(&record) != OTA_CONTROL_OK)
        {
            Debug_LogLine(DEBUG_SRC_SYSTEM,
                          "OTA: factory anti-rollback floor write failed");
            return -1;
        }
        OtaManager_LogRecord("factory-confirmed", &record);
        return 0;
    }
    if (result != OTA_CONTROL_OK)
    {
        Debug_LogLine(DEBUG_SRC_SYSTEM, "OTA: control log invalid");
        return -1;
    }

    OtaManager_LogRecord("boot", &record);
    if (record.state == OTA_CONTROL_STATE_TRIAL)
    {
        ota_trial_active = 1U;
        ota_trial_deadline_ms = HAL_GetTick() + OTA_HEALTH_TIMEOUT_MS;
    }
    return 0;
}

int OtaManager_ConfirmHealthy(void)
{
    OtaControlRecord record;
    OtaImageDescriptor verified;

    if (OtaControl_LoadLatest(&record) != OTA_CONTROL_OK)
    {
        return ota_trial_active == 0U ? 0 : -1;
    }
    if (record.state != OTA_CONTROL_STATE_TRIAL)
    {
        ota_trial_active = 0U;
        return 0;
    }
    if ((OtaImage_VerifySlot(OTA_PRIMARY_SLOT_ADDRESS, &verified) !=
         OTA_IMAGE_OK) ||
        (memcmp(&verified, &record.primary, sizeof(verified)) != 0))
    {
        Debug_LogLine(DEBUG_SRC_SYSTEM,
                      "OTA: trial confirmation rejected, image mismatch");
        return -1;
    }

    record.state = OTA_CONTROL_STATE_CONFIRMED;
    record.confirmed_version = verified.image_version;
    record.trial_boot_count = 0U;
    record.swap_sector = 0U;
    record.swap_step = 0U;
    record.flags |= OTA_CONTROL_FLAG_JOB_REPORT_PENDING;
    if (OtaControl_Append(&record) != OTA_CONTROL_OK)
    {
        Debug_LogLine(DEBUG_SRC_SYSTEM,
                      "OTA: trial confirmation write failed");
        return -1;
    }

    ota_trial_active = 0U;
    ota_trial_deadline_ms = 0U;
    OtaManager_LogRecord("confirmed", &record);
    return 0;
}

int OtaManager_RequestInstall(void)
{
    OtaControlRecord record;

    if ((OtaControl_LoadLatest(&record) != OTA_CONTROL_OK) ||
        (record.state != OTA_CONTROL_STATE_CANDIDATE_READY) ||
        !OtaControl_ImageDescriptorIsValid(&record.secondary) ||
        (record.secondary.image_version <= record.confirmed_version))
    {
        Debug_LogLine(DEBUG_SRC_SYSTEM,
                      "OTA: install request rejected, no valid candidate");
        return -1;
    }

    Debug_LogLine(DEBUG_SRC_SYSTEM,
                  "OTA: verified candidate handed to bootloader");
    __DSB();
    NVIC_SystemReset();
    return -1;
}

void OtaManager_Process(uint32_t now_ms)
{
    if ((ota_trial_active != 0U) &&
        ((int32_t)(now_ms - ota_trial_deadline_ms) >= 0))
    {
        Debug_LogLine(DEBUG_SRC_SYSTEM,
                      "OTA: trial health timeout, rebooting for rollback");
        __DSB();
        NVIC_SystemReset();
    }
}

int OtaManager_GetPendingJob(OtaPendingJob *job)
{
    OtaControlRecord record;

    if ((job == NULL) ||
        (OtaControl_LoadLatest(&record) != OTA_CONTROL_OK) ||
        ((record.flags & OTA_CONTROL_FLAG_JOB_REPORT_PENDING) == 0U) ||
        (record.job_id[0] == '\0'))
    {
        return 0;
    }

    memset(job, 0, sizeof(*job));
    (void)snprintf(job->job_id, sizeof(job->job_id), "%s", record.job_id);
    job->execution_version = record.job_execution_version;
    job->image_version = record.primary.image_version;
    job->rejected =
        (record.flags & OTA_CONTROL_FLAG_INSTALL_REJECTED) != 0U;
    job->succeeded =
        ((record.flags & OTA_CONTROL_FLAG_ROLLBACK_PERFORMED) == 0U) &&
        !job->rejected;
    return 1;
}

int OtaManager_GetReadyInstallJob(OtaPendingJob *job)
{
    OtaControlRecord record;

    if ((job == NULL) ||
        (OtaControl_LoadLatest(&record) != OTA_CONTROL_OK) ||
        (record.state != OTA_CONTROL_STATE_CANDIDATE_READY) ||
        (record.job_id[0] == '\0'))
    {
        return 0;
    }

    memset(job, 0, sizeof(*job));
    (void)snprintf(job->job_id, sizeof(job->job_id), "%s", record.job_id);
    job->execution_version = record.job_execution_version;
    job->image_version = record.secondary.image_version;
    job->succeeded = 1;
    job->rejected = 0;
    return 1;
}

int OtaManager_UpdateJobExecutionVersion(uint32_t execution_version)
{
    OtaControlRecord record;

    if ((execution_version == 0U) ||
        (OtaControl_LoadLatest(&record) != OTA_CONTROL_OK) ||
        (record.state != OTA_CONTROL_STATE_CANDIDATE_READY) ||
        (record.job_id[0] == '\0') ||
        (execution_version < record.job_execution_version))
    {
        return -1;
    }
    if (execution_version == record.job_execution_version)
    {
        return 0;
    }

    record.job_execution_version = execution_version;
    return OtaControl_Append(&record) == OTA_CONTROL_OK ? 0 : -1;
}

int OtaManager_MarkJobReported(void)
{
    OtaControlRecord record;

    if ((OtaControl_LoadLatest(&record) != OTA_CONTROL_OK) ||
        ((record.flags & OTA_CONTROL_FLAG_JOB_REPORT_PENDING) == 0U))
    {
        return -1;
    }
    record.flags &= ~(OTA_CONTROL_FLAG_JOB_REPORT_PENDING |
                      OTA_CONTROL_FLAG_ROLLBACK_PERFORMED |
                      OTA_CONTROL_FLAG_INSTALL_REJECTED);
    memset(record.job_id, 0, sizeof(record.job_id));
    record.job_execution_version = 0U;
    return OtaControl_Append(&record) == OTA_CONTROL_OK ? 0 : -1;
}

int OtaManager_IsTrial(void)
{
    return ota_trial_active != 0U;
}
