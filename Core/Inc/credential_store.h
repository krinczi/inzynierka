#ifndef INC_CREDENTIAL_STORE_H_
#define INC_CREDENTIAL_STORE_H_

#include <stddef.h>
#include <stdint.h>

#define CREDENTIAL_STORE_CERTIFICATE_PEM_MAX_LENGTH  2304U
#define CREDENTIAL_STORE_PRIVATE_KEY_PEM_MAX_LENGTH  2048U
#define CREDENTIAL_STORE_OWNERSHIP_TOKEN_MAX_LENGTH  2048U
#define CREDENTIAL_STORE_CERTIFICATE_ID_MAX_LENGTH    128U
#define CREDENTIAL_STORE_THING_NAME_MAX_LENGTH        128U

typedef enum
{
    CREDENTIAL_STORE_EMPTY = 0,
    CREDENTIAL_STORE_PENDING_REGISTRATION = 1,
    CREDENTIAL_STORE_PROVISIONED = 2,
    CREDENTIAL_STORE_ROTATION_REQUESTED = 3,
    CREDENTIAL_STORE_ROTATION_PENDING = 4,
    CREDENTIAL_STORE_ROTATION_VALIDATING = 5
} CredentialStoreState;

typedef struct
{
    char certificate_pem[CREDENTIAL_STORE_CERTIFICATE_PEM_MAX_LENGTH + 1U];
    char private_key_pem[CREDENTIAL_STORE_PRIVATE_KEY_PEM_MAX_LENGTH + 1U];
    char ownership_token[CREDENTIAL_STORE_OWNERSHIP_TOKEN_MAX_LENGTH + 1U];
    char certificate_id[CREDENTIAL_STORE_CERTIFICATE_ID_MAX_LENGTH + 1U];
    char thing_name[CREDENTIAL_STORE_THING_NAME_MAX_LENGTH + 1U];
} CredentialStoreData;

/* Loads and verifies the newest valid dual-slot record from internal Flash. */
int CredentialStore_Init(void);

CredentialStoreState CredentialStore_GetState(void);
const CredentialStoreData *CredentialStore_Get(void);
const char *CredentialStore_StateName(CredentialStoreState state);
int CredentialStore_UsesDeviceCredentials(void);
int CredentialStore_IsProvisioningRequired(void);
int CredentialStore_IsRotationInProgress(void);

/* Atomically stores credentials returned by CreateKeysAndCertificate. */
int CredentialStore_SavePending(const char *certificate_pem,
                                const char *private_key_pem,
                                const char *ownership_token,
                                const char *certificate_id);

/* Atomically commits the Thing name after RegisterThing succeeds. */
int CredentialStore_CommitProvisioned(const char *thing_name);

/* Starts a claim-based rotation while retaining the last provisioned record. */
int CredentialStore_RequestRotation(void);

/* Promotes a rotated pair only after a stable TLS/MQTT validation session. */
int CredentialStore_CommitRotationValidated(void);

/* Restores the most recent provisioned pair retained in the journal. */
int CredentialStore_RollbackRotation(void);

/*
 * Persists one failed rotated-credential validation. Returns 1 when the
 * failure limit caused an automatic rollback, 0 when another retry is
 * permitted, and -1 on a storage error.
 */
int CredentialStore_RecordRotationFailure(void);

/* Factory/service operation. Erases both credential slots. */
int CredentialStore_Erase(void);

#endif /* INC_CREDENTIAL_STORE_H_ */
