#ifndef INC_FLASH_STAGING_SELF_TEST_H_
#define INC_FLASH_STAGING_SELF_TEST_H_

#include "flash_staging.h"

/*
 * Runs a destructive write/read test only when staging is empty, invalid, or
 * already contains this module's known test payload. A different READY file
 * is never overwritten.
 */
FlashStagingResult FlashStagingSelfTest_Run(void);

#endif /* INC_FLASH_STAGING_SELF_TEST_H_ */
