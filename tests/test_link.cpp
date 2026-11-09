/* Links in editor text: which address is under a byte offset (Ctrl+click opens it). */
#include <gtest/gtest.h>
#include "../src/link.h"

TEST(Link, FindsAnAddressUnderTheOffset) {
    std::string l = "see https://example.com/a?b=1 for more";
    EXPECT_EQ(link_at(l, 4), "https://example.com/a?b=1");
    EXPECT_EQ(link_at(l, 20), "https://example.com/a?b=1");
    EXPECT_EQ(link_at(l, 3), "");
    EXPECT_EQ(link_at(l, 30), "");
}
TEST(Link, TrimsTrailingPunctuationAndStopsAtQuotes) {
    EXPECT_EQ(link_at("(http://a.org/x).", 5), "http://a.org/x");
    EXPECT_EQ(link_at("href=\"ftp://h/f.txt\">", 8), "ftp://h/f.txt");
    EXPECT_EQ(link_at("mail mailto:me@x.org, ok", 7), "mailto:me@x.org");
}
TEST(Link, IgnoresBareSchemesAndOtherText) {
    EXPECT_EQ(link_at("http:// alone", 2), "");
    EXPECT_EQ(link_at("no links here", 3), "");
    EXPECT_EQ(link_at("", 0), "");
}
TEST(Link, TwoOnOneLine) {
    std::string l = "http://a.io https://b.io";
    EXPECT_EQ(link_at(l, 2), "http://a.io");
    EXPECT_EQ(link_at(l, 15), "https://b.io");
}
