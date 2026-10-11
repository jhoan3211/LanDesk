#include <gtest/gtest.h>

#include "lancast/core/json_writer.hpp"
#include "lancast/core/report.hpp"

using lancast::JsonWriter;

TEST(JsonWriter, EscapesSpecialCharacters) {
    EXPECT_EQ(JsonWriter::escape("a\"b\\c\n"), R"("a\"b\\c\n")");
    EXPECT_EQ(JsonWriter::escape(std::string_view{"\x01", 1}), R"("\u0001")");
    EXPECT_EQ(JsonWriter::escape("\\\\.\\DISPLAY1"), R"("\\\\.\\DISPLAY1")");
    EXPECT_EQ(JsonWriter::escape("Señal"), "\"Señal\"");
}

TEST(JsonWriter, CompactObjectsAndArrays) {
    JsonWriter j{0};
    j.begin_object()
        .field("n", std::int32_t{-3})
        .field("ok", true)
        .field("s", "x")
        .key("list")
        .begin_array()
        .value(std::uint32_t{1})
        .value(std::uint32_t{2})
        .end_array()
        .key("empty")
        .begin_object()
        .end_object()
        .key("none")
        .null()
        .end_object();
    EXPECT_EQ(j.str(), R"({"n":-3,"ok":true,"s":"x","list":[1,2],"empty":{},"none":null})");
}

TEST(JsonWriter, ReportJsonContainsAllSections) {
    lancast::SystemReport report;
    report.gpus.push_back(lancast::GpuInfo{.index = 0, .name = "GPU \"A\"", .vendor = lancast::GpuVendor::Amd});
    const std::string json = lancast::format_report_json(report);
    for (const char* key : {"\"gpus\"", "\"monitors\"", "\"virtual_desktop\"", "\"encoders\"", "\"decoders\"",
                            "\"warnings\"", "GPU \\\"A\\\""}) {
        EXPECT_NE(json.find(key), std::string::npos) << key;
    }
}
