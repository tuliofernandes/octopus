#include "octopus/completion.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("stop detector removes stop completed across chunks",
          "[completion]") {
  octopus::StopDetector detector({"<end_of_turn>"});

  CHECK_FALSE(detector.append("hello <end"));
  CHECK(detector.text() == "hello <end");

  CHECK_FALSE(detector.append("_of"));
  CHECK(detector.text() == "hello <end_of");

  CHECK(detector.append("_turn>"));
  CHECK(detector.text() == "hello ");
}

TEST_CASE("stop detector retains partial stop prefixes until resolved",
          "[completion]") {
  octopus::StopDetector detector({"<end_of_turn>"});

  CHECK_FALSE(detector.append("alpha <end"));
  CHECK(detector.text() == "alpha <end");

  CHECK_FALSE(detector.append("x"));
  CHECK(detector.text() == "alpha <endx");
}

TEST_CASE("stop detector ignores empty stop strings", "[completion]") {
  octopus::StopDetector detector({"", "<stop>"});

  CHECK_FALSE(detector.append("answer"));
  CHECK(detector.text() == "answer");

  CHECK(detector.append("<stop>"));
  CHECK(detector.text() == "answer");
}

TEST_CASE("stop detector deduplicates stop strings", "[completion]") {
  octopus::StopDetector detector({"<stop>", "", "<stop>", "<other>"});

  REQUIRE(detector.stopStrings().size() == 2);
  CHECK(detector.stopStrings()[0] == "<stop>");
  CHECK(detector.stopStrings()[1] == "<other>");
}

TEST_CASE("stop-safe text buffer holds possible stop suffixes",
          "[completion]") {
  octopus::StopSafeTextBuffer buffer({"<stop>"});

  CHECK(buffer.append("abcd") == "");
  CHECK(buffer.append("ef") == "a");
  CHECK(buffer.flush("abcdef") == "bcdef");
}

TEST_CASE("stop-safe text buffer does not flush stop markers", "[completion]") {
  octopus::StopSafeTextBuffer buffer({"<stop>"});

  std::string visible;
  visible += buffer.append("answer<st");
  visible += buffer.flush("answer");

  CHECK(visible == "answer");
}

TEST_CASE("stop-safe text buffer streams immediately without stops",
          "[completion]") {
  octopus::StopSafeTextBuffer buffer({});

  CHECK(buffer.append("alpha") == "alpha");
  CHECK(buffer.append(" beta") == " beta");
  CHECK(buffer.flush("alpha beta") == "");
}

TEST_CASE("loop detector catches repeated identical short lines",
          "[completion]") {
  octopus::LoopDetector detector;

  CHECK_FALSE(detector.append("ready\n"));
  CHECK_FALSE(detector.append("ready\n"));
  CHECK_FALSE(detector.append("ready\n"));

  CHECK(detector.append("ready\n"));
  CHECK(detector.trimSize() == 6);
}

TEST_CASE("loop detector catches repeated n-gram windows conservatively",
          "[completion]") {
  octopus::LoopDetector detector;

  CHECK_FALSE(detector.append("alpha beta gamma "));
  CHECK_FALSE(detector.append("alpha beta gamma "));
  CHECK_FALSE(detector.append("alpha beta gamma "));

  CHECK(detector.append("alpha beta gamma "));
  CHECK(detector.trimSize() == 17);
}

TEST_CASE("loop detector ignores normal short answers", "[completion]") {
  octopus::LoopDetector detector;

  CHECK_FALSE(detector.append("Ready."));
  CHECK_FALSE(detector.append(" The command completed successfully."));
  CHECK_FALSE(detector.detected());
  CHECK(detector.trimSize() == detector.generatedSize());
}
