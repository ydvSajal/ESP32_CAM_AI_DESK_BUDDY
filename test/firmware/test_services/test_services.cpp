// Native unit tests for the pure logic behind A2's core-1 services.
// No Arduino, no hardware, no network: uploader.h / ai.h expose these as inline functions
// precisely so they can be checked on a laptop.
//
//   pio test -e native
//
// (requires `test_dir = ../test/firmware` in [env:native] — see the A2 handover note)
#include <unity.h>
#include <string>

#include "uploader.h"
#include "ai.h"

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- Drive paths

static void test_drive_day_from_path() {
  TEST_ASSERT_EQUAL_STRING("20260802", driveDayFromPath("/rec/20260802/003000.avi").c_str());
  TEST_ASSERT_EQUAL_STRING("", driveDayFromPath("/rec/003000.avi").c_str());       // no day dir
  TEST_ASSERT_EQUAL_STRING("", driveDayFromPath("/rec/2026080/003000.avi").c_str());  // 7 digits
  TEST_ASSERT_EQUAL_STRING("", driveDayFromPath("/rec/2026080x/003000.avi").c_str()); // not numeric
  TEST_ASSERT_EQUAL_STRING("", driveDayFromPath("").c_str());
}

static void test_drive_folder_path() {
  TEST_ASSERT_EQUAL_STRING("DeskBuddy/20260802",
                           driveFolderPath("/rec/20260802/003000.avi").c_str());
  // Unshaped path still lands somewhere sane rather than at the Drive root.
  TEST_ASSERT_EQUAL_STRING("DeskBuddy", driveFolderPath("/junk.avi").c_str());
}

static void test_drive_file_name() {
  TEST_ASSERT_EQUAL_STRING("003000.avi", driveFileName("/rec/20260802/003000.avi").c_str());
  TEST_ASSERT_EQUAL_STRING("x.avi", driveFileName("x.avi").c_str());
}

// ---------------------------------------------------------------- backoff

static void test_backoff_schedule() {
  TEST_ASSERT_EQUAL_UINT32(5000, backoffMs(0));
  TEST_ASSERT_EQUAL_UINT32(10000, backoffMs(1));
  TEST_ASSERT_EQUAL_UINT32(20000, backoffMs(2));
  TEST_ASSERT_EQUAL_UINT32(160000, backoffMs(5));
  TEST_ASSERT_EQUAL_UINT32(300000, backoffMs(6));    // 320 s would exceed the cap
  TEST_ASSERT_EQUAL_UINT32(300000, backoffMs(99));   // no overflow, no wraparound to 0
  // Monotonic until the cap — a schedule that ever goes backwards is a retry storm.
  for (uint32_t i = 1; i < 12; i++) TEST_ASSERT_TRUE(backoffMs(i) >= backoffMs(i - 1));
}

// ---------------------------------------------------------------- resumable protocol

static void test_range_and_content_range() {
  TEST_ASSERT_EQUAL_UINT64(262144, nextOffsetFromRange("bytes=0-262143"));
  TEST_ASSERT_EQUAL_UINT64(1, nextOffsetFromRange("bytes=0-0"));
  TEST_ASSERT_EQUAL_UINT64(0, nextOffsetFromRange(""));     // Drive has nothing yet
  TEST_ASSERT_EQUAL_STRING("bytes 0-262143/1000000", contentRange(0, 262144, 1000000).c_str());
  TEST_ASSERT_EQUAL_STRING("bytes 262144-262244/262245",
                           contentRange(262144, 101, 262245).c_str());
}

// ---------------------------------------------------------------- JSON building

static void test_json_escape() {
  TEST_ASSERT_EQUAL_STRING("a\\\"b", jsonEscape("a\"b").c_str());
  TEST_ASSERT_EQUAL_STRING("a\\\\b", jsonEscape("a\\b").c_str());
  TEST_ASSERT_EQUAL_STRING("l1\\nl2", jsonEscape("l1\nl2").c_str());
  TEST_ASSERT_EQUAL_STRING("\\u0001", jsonEscape(std::string(1, '\x01')).c_str());
}

static void test_request_body_openrouter() {
  std::string b = aiRequestBody("openrouter", "openai/gpt-4o-mini", "what is on my desk?");
  TEST_ASSERT_TRUE(b.find("\"model\":\"openai/gpt-4o-mini\"") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("\"stream\":true") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("\"content\":\"what is on my desk?\"") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("contents") == std::string::npos);   // not the Gemini shape
}

static void test_request_body_gemini() {
  std::string b = aiRequestBody("gemini", "ignored", "hi \"there\"");
  TEST_ASSERT_TRUE(b.find("\"contents\"") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("\"text\":\"hi \\\"there\\\"\"") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("messages") == std::string::npos);   // not the OpenAI shape
}

// ---------------------------------------------------------------- SSE parsing

static void test_sse_openrouter() {
  TEST_ASSERT_EQUAL_STRING(
      " and a small",
      sseExtractText("openrouter",
                     "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\","
                     "\"content\":\" and a small\"}}]}").c_str());
  // Final delta carries content:null — must not be mistaken for text.
  TEST_ASSERT_EQUAL_STRING(
      "", sseExtractText("openrouter",
                         "data: {\"choices\":[{\"delta\":{\"content\":null},"
                         "\"finish_reason\":\"stop\"}]}").c_str());
  TEST_ASSERT_EQUAL_STRING("", sseExtractText("openrouter", "data: [DONE]").c_str());
  TEST_ASSERT_EQUAL_STRING("", sseExtractText("openrouter", ": OPENROUTER PROCESSING").c_str());
  TEST_ASSERT_EQUAL_STRING("", sseExtractText("openrouter", "").c_str());
}

static void test_sse_gemini() {
  TEST_ASSERT_EQUAL_STRING(
      "hello",
      sseExtractText("gemini",
                     "data: {\"candidates\":[{\"content\":{\"role\":\"model\","
                     "\"parts\":[{\"text\":\"hello\"}]}}]}").c_str());
  // Escapes survive the round trip, and a trailing \r from the wire is stripped.
  TEST_ASSERT_EQUAL_STRING(
      "a\nB",
      sseExtractText("gemini", "data: {\"parts\":[{\"text\":\"a\\n\\u0042\"}]}\r").c_str());
}

// ---------------------------------------------------------------- T8: masking

static void test_redact_secret() {
  TEST_ASSERT_EQUAL_STRING(
      "provider rejected *** badly",
      redactSecret("provider rejected sk-or-v1-abcdef badly", "sk-or-v1-abcdef").c_str());
  // Every occurrence, not just the first.
  TEST_ASSERT_EQUAL_STRING("*** and ***", redactSecret("SECRETK and SECRETK", "SECRETK").c_str());
  // Nothing to hide: text is returned untouched.
  TEST_ASSERT_EQUAL_STRING("all good", redactSecret("all good", "SECRETK").c_str());
  // A too-short "secret" would shred ordinary prose — refuse it.
  TEST_ASSERT_EQUAL_STRING("abc def", redactSecret("abc def", "ab").c_str());
  TEST_ASSERT_EQUAL_STRING("abc def", redactSecret("abc def", "").c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_drive_day_from_path);
  RUN_TEST(test_drive_folder_path);
  RUN_TEST(test_drive_file_name);
  RUN_TEST(test_backoff_schedule);
  RUN_TEST(test_range_and_content_range);
  RUN_TEST(test_json_escape);
  RUN_TEST(test_request_body_openrouter);
  RUN_TEST(test_request_body_gemini);
  RUN_TEST(test_sse_openrouter);
  RUN_TEST(test_sse_gemini);
  RUN_TEST(test_redact_secret);
  return UNITY_END();
}
