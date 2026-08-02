#pragma once
// webserver.h — Owner: A2. Runs on CORE_SERVICES (core 1), netTask prio 3.
// ESPAsyncWebServer routes + /ws + captive setup portal. THE contract is docs/API.md —
// implement it exactly; it is frozen and A3 codes the PWA against it blind.
#include <Arduino.h>

extern "C" void netTask(void* pv);

void webserverBegin(bool setupMode);     // setupMode = SoftAP captive portal
void wsBroadcast(const char* type, const String& payloadJson);   // docs/API.md envelope
String statusJson();                     // GET /api/status body, also the ws "status" payload

// docs/API.md 4 `error` envelope. source ∈ camera|recorder|uploader|ai|wifi|sd|system.
// Callable from ANY task: the frame is queued and only netTask touches AsyncWebSocket.
void wsError(const char* source, const char* code, const String& message, bool fatal);
