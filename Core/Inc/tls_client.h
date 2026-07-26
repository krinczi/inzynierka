/*
 * tls_client.h
 *
 *  Created on: Apr 11, 2026
 *      Author: justy
 */

#ifndef INC_TLS_CLIENT_H_
#define INC_TLS_CLIENT_H_

#include <stdint.h>
#include <stddef.h>

int TlsClient_Init(void);
int TlsClient_Connect(void);
int TlsClient_Write(const uint8_t *data, size_t len);
int TlsClient_Read(uint8_t *data, size_t len, uint32_t timeout_ms);
int TlsClient_MqttTest(void);
int TlsClient_MqttProcess(void);
int TlsClient_MqttIsReady(void);
/* Thread-safe cancellation flag used by the Ethernet supervisor. */
void TlsClient_RequestCancel(void);
void TlsClient_ClearCancel(void);
void TlsClient_Disconnect(void);
#endif /* INC_TLS_CLIENT_H_ */
