# Architektura runtime firmware STM32H753

Stan: **opis aktualnego podziału odpowiedzialności, 2026-08-24**.

## Uruchomienie

1. `main()` inicjalizuje HAL, zegary, GPIO, RNG, UART i nieblokujący logger.
2. Tworzone są `defaultTask` oraz `mqttService`.
3. `defaultTask` uruchamia LwIP dokładnie raz, następnie RTC, magazyny trwałe,
   TLS, security posture i IWDG.
4. Sieć jest konfigurowana po pojawieniu się linku: DHCP albo zwalidowane static.
5. `mqttService` jest jedynym właścicielem kontekstów Mbed TLS/MQTT.
6. Po ważnym czasie, IP i TLS urządzenie ustanawia MQTT, subskrypcje, Shadow,
   Jobs i provisioning zależnie od stanu poświadczeń.

Usługi trwałe inicjalizują się bez Ethernetu. Dzięki temu konfiguracja,
poświadczenia, OTA control i outbox są sprawdzane również podczas offline boot.

## Taski i własność zasobów

| Task | Stos | Odpowiedzialność |
| --- | ---: | --- |
| `defaultTask` | 16 KiB | LwIP init, link/DHCP/static, SNTP, network supervisor, IWDG service |
| `mqttService` | 64 KiB | TLS, MQTT, Fleet Provisioning, Shadow, Jobs, HTTPS state machine, OTA health gate |
| logger | 768 słów | kolejki logów i UART transmit IT |
| LwIP internal | według konfiguracji LwIP | tcpip thread, RX/input i timery protokołu |

Konteksty TLS i duże bufory MQTT są statyczne i używane przez jednego
właściciela. Callback Ethernet nie wykonuje logowania ani operacji Flash.

## Watchdog

IWDG ma timeout około 32 s. Jest odświeżany tylko wtedy, gdy supervisor sieci i
task MQTT zgłaszają świeże heartbeat. Długie operacje mają timeouty, kroki
maszyny stanów lub jawne odświeżanie watchdoga w bootloaderze. Reset IWDG jest
raportowany podczas następnego startu.

Watchdog odzyskuje urządzenie z zawieszenia, ale nie dowodzi poprawności danych.
Integralność zapewniają osobno CRC, SHA-256, podpisy, read-back i commit-last.

## Sieć i reconnect

- domyślnie DHCP z timeoutem 30 s i retry co 5 s;
- static wymaga poprawnego hosta, maski, gateway i DNS;
- po trzech nieudanych cloud-health próbach opcjonalny fallback używa DHCP;
- utrata linku anuluje aktywne oczekiwanie TLS i odkłada reconnect;
- TLS connect ma timeout 10 s, handshake 60 s, a pętla oddaje czas RTOS;
- MQTT Last Will/status daje chmurze retained `offline` po zerwaniu sesji;
- po reconnect odtwarzane są Shadow, Jobs i trwałe wiadomości.

## Bufory i limity

Największe bufory MQTT mają 8 KiB RX, 16 KiB TX i 15 KiB katalogu capability.
Incoming payload jest ograniczony, parser JSON ma limity długości i głębokości,
a każdy topic mieści maksymalnie 256 znaków. FreeRTOS heap ma 192 KiB.

Build musi kontrolować mapę linkera. Bufor zwiększony w jednym module może
odebrać miejsce stosom lub LwIP nawet wtedy, gdy kompilator nie zgłosi błędu.
W teście długotrwałym należy zbierać minimum free heap, high-water mark każdego
taska, liczniki RX allocation failure oraz resety IWDG/HardFault.

## Logger

Każde źródło logów ma kolejkę o głębokości 48. Producer używa timeoutu 0, więc
przy przeciążeniu log jest odrzucany zamiast blokować task sieciowy. UART używa
transmisji przerwaniowej z ograniczonym timeoutem i licznikiem dropów. Szczegóły
TLS state/IO oraz okresowe heartbeat są wyłączane w profilu produkcyjnym.

Bezpośredni polling UART pozostaje tylko w ścieżce HardFault, kiedy normalny
scheduler nie jest wiarygodny.

## Flash i współbieżność

- poświadczenia i konfiguracja używają dwóch sektorów/generacji;
- OTA staging zapisuje Bank 2, gdy aplikacja działa z Banku 1;
- bootloader wykonuje krytyczne swap routines z RAM;
- OTA control ma dziennik i scratch backup;
- outbox ma jeden sektor i nie gwarantuje atomowej kompakcji podczas power-cut.

Kod nie może wykonywać dwóch writerów tego samego regionu Flash. Każda nowa
funkcja trwała musi najpierw otrzymać jawny obszar w linkerze, właściciela,
format, recovery scan i test przerwania zasilania.

## Warstwa bezpieczeństwa

- TLS 1.2 z CA, hostname/SNI i unikalnym certyfikatem klienta;
- hardware RNG jako źródło entropii;
- ważny RTC/SNTP przed TLS;
- ECDSA P-256 dla OTA i osobny transport SHA-256;
- MPU ustawia pamięć persistent jako XN/non-cacheable;
- wszystkie taski pozostają uprzywilejowane w aktualnym porcie non-MPU;
- RDP/WRP/PCROP nie są programowane przez aplikację;
- prywatny klucz urządzenia w internal Flash wymaga RDP1 albo secure elementu
  zgodnie z modelem zagrożeń produktu.

## Reguły rozbudowy

1. Nie inicjalizować LwIP ponownie w adapterze TLS/HTTPS.
2. Nie wykonywać nieograniczonego oczekiwania w tasku MQTT ani tcpip thread.
3. Nie logować z ISR i nie logować sekretów/presigned URL.
4. Nie przyjmować adresów Flash, nazw topiców ani długości bezpośrednio z chmury.
5. Każda komenda musi mieć walidację, idempotencję i jawny wynik.
6. Każdy trwały format musi mieć wersję, CRC/integralność i migrację lub
   bezpieczne odrzucenie.

