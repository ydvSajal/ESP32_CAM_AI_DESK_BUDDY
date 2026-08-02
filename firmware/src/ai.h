#pragma once
// ai.h — Owner: A2. Runs on CORE_SERVICES (core 1), aiTask prio 2, queue depth 1.
// OpenRouter + Gemini behind one interface. Implement per HANDOVER.md section 4;
// streams ai_chunk / ai_done over /ws (docs/API.md 4).
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef ARDUINO
#include <Arduino.h>

extern "C" void aiTask(void* pv);

bool aiBusy();
// Returns false if busy (webserver answers 409). `id` is the 4-char handle echoed on /ws.
bool aiSubmit(const String& prompt, const String& provider, bool stream, String& id);

// --- added by A2, used by webserver.cpp / display.cpp ---
bool aiHasKey(const String& provider);          // false => 503 "no api key for provider X"
String aiEffectiveProvider(const String& want);  // "" -> Settings.aiProvider

// Blocking-mode helper for POST /api/ai with stream:false. Waits up to timeoutMs for the
// in-flight request `id`. Returns false on timeout. `error` empty => success.
bool aiWaitResult(const String& id, String& answer, String& error, uint32_t& elapsedMs, uint32_t timeoutMs);

// Last question/answer for A4's AI display screen. Thread-safe (short mutex hold).
void aiLastQA(String& question, String& answer);
#endif  // ARDUINO

// =====================================================================
//  Pure helpers — no Arduino, no hardware, no ArduinoJson. Unit-tested in
//  test/firmware/test_services under [env:native].
// =====================================================================

inline std::string jsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  return o;
}

inline std::string jsonUnescape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
    char n = s[++i];
    switch (n) {
      case 'n': o += '\n'; break;
      case 'r': o += '\r'; break;
      case 't': o += '\t'; break;
      case 'b': o += '\b'; break;
      case 'f': o += '\f'; break;
      case 'u': {
        if (i + 4 >= s.size()) return o;
        unsigned cp = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
        i += 4;
        // ponytail: encodes the BMP code point as UTF-8; surrogate pairs are emitted as
        // two replacement-ish sequences. Upgrade to pair-joining if a provider ever
        // streams astral-plane text (emoji) and the PWA shows mojibake.
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        break;
      }
      default: o += n;
    }
  }
  return o;
}

// Value of the first `"key": "..."` string in `j`, unescaped. "" when absent or non-string
// (e.g. OpenRouter's final chunk sends "content":null).
inline std::string jsonFindString(const std::string& j, const std::string& key) {
  std::string pat = "\"" + key + "\"";
  size_t k = j.find(pat);
  if (k == std::string::npos) return "";
  size_t i = j.find(':', k + pat.size());
  if (i == std::string::npos) return "";
  i++;
  while (i < j.size() && (j[i] == ' ' || j[i] == '\t')) i++;
  if (i >= j.size() || j[i] != '"') return "";   // null / number / object
  i++;
  std::string raw;
  for (; i < j.size(); i++) {
    if (j[i] == '\\') { raw += j[i]; if (i + 1 < j.size()) raw += j[++i]; continue; }
    if (j[i] == '"') break;
    raw += j[i];
  }
  return jsonUnescape(raw);
}

// Request body for either provider. Always asks for a stream — aiTask uses one code path
// and simply accumulates instead of forwarding when the client did not ask for chunks.
inline std::string aiRequestBody(const std::string& provider, const std::string& model,
                                 const std::string& prompt) {
  std::string p = jsonEscape(prompt);
  if (provider == "gemini")
    return "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"" + p + "\"}]}]}";
  return "{\"model\":\"" + jsonEscape(model) +
         "\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"" + p + "\"}]}";
}

// One SSE line -> the text fragment it carries ("" for keep-alives, comments, [DONE]).
inline std::string sseExtractText(const std::string& provider, const std::string& line) {
  if (line.rfind("data:", 0) != 0) return "";     // ": OPENROUTER PROCESSING" etc.
  std::string j = line.substr(5);
  size_t b = j.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  j = j.substr(b);
  while (!j.empty() && (j.back() == '\r' || j.back() == '\n')) j.pop_back();
  if (j == "[DONE]") return "";
  return jsonFindString(j, provider == "gemini" ? "text" : "content");
}

// T8: nothing that ever leaves the device may contain a key. Applied to every error string
// and every serial line. Secrets shorter than 4 chars are not redacted (would nuke the text).
inline std::string redactSecret(const std::string& text, const std::string& secret) {
  if (secret.size() < 4) return text;
  std::string o = text;
  size_t p = 0;
  while ((p = o.find(secret, p)) != std::string::npos) { o.replace(p, secret.size(), "***"); p += 3; }
  return o;
}
