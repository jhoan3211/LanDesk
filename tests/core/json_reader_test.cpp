#include <gtest/gtest.h>

#include "lancast/core/json_reader.hpp"
#include "lancast/core/version.hpp"

using namespace lancast;

TEST(JsonReader, ParsesGitHubReleaseShape) {
    const auto v = json::parse(R"({
        "tag_name": "v1.3.0",
        "body": "Novedades\r\nSHA-256: abc123",
        "draft": false,
        "assets": [ { "name": "LanCast.exe", "size": 1814528,
                      "browser_download_url": "https://github.com/x/y/releases/download/v1.3.0/LanCast.exe" } ]
    })");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ((*v)["tag_name"].string_or(""), "v1.3.0");
    EXPECT_EQ((*v)["draft"].boolean(), false);
    const json::Array* assets = (*v)["assets"].array();
    ASSERT_NE(assets, nullptr);
    ASSERT_EQ(assets->size(), 1u);
    EXPECT_EQ((*assets)[0]["size"].number(), 1814528.0);
    EXPECT_TRUE((*v)["no_existe"].is_null());
    EXPECT_TRUE((*v)["no_existe"]["tampoco"].is_null());
}

TEST(JsonReader, UnicodeEscapesAndRejectsGarbage) {
    const auto v = json::parse(R"(["ññ😀", -1.5e3, true, null])");
    ASSERT_TRUE(v.has_value());
    const json::Array& a = *v->array();
    EXPECT_EQ(*a[0].string(), "ññ\xF0\x9F\x98\x80");
    EXPECT_EQ(a[1].number(), -1500.0);
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "tru", "\"sin cerrar", "[1] extra", "{\"a\":01x}"}) {
        EXPECT_FALSE(json::parse(bad).has_value()) << bad;
    }
    std::string deep(200, '[');
    EXPECT_FALSE(json::parse(deep).has_value());
}

TEST(Version, ParseAndCompare) {
    EXPECT_EQ(parse_version("v1.3.0"), (Version{1, 3, 0}));
    EXPECT_EQ(parse_version("1.10.2"), (Version{1, 10, 2}));
    EXPECT_FALSE(parse_version("latest").has_value());
    EXPECT_LT((Version{1, 2, 9}), (Version{1, 3, 0}));
    EXPECT_LT((Version{1, 9, 0}), (Version{1, 10, 0}));
    EXPECT_EQ(to_string(Version{1, 3, 0}), "1.3.0");
}
