#include <vcpkg-test/util.h>

#include <vcpkg/base/diagnostics.h>
#include <vcpkg/base/parse.h>
#include <vcpkg/base/unicode.h>

using namespace vcpkg;
using namespace vcpkg::Unicode;

static std::string marker_at(const ParseEnumerator& parser)
{
    FullyBufferedDiagnosticContext context;
    parser.report_error_with_caret_line(context, LocalizedString::from_raw("marker"));
    return context.to_string();
}

TEST_CASE ("ParsedDocument owns copied input", "[parse]")
{
    std::string source = "a\xED\xA0\xBC";
    ParsedDocument doc(source, StringView{"copied.txt"});
    source[0] = 'z';

    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    REQUIRE(parser.next(context) == U'a');
    REQUIRE(parser.next(context) == 0xD83C);
    REQUIRE(parser.next(context) == end_of_file);
    REQUIRE(context.empty());
}

TEST_CASE ("ParsedEnumerator next decodes valid Unicode edge cases", "[parse]")
{
    ParsedDocument doc(StringView{"a\t\xC3\xA9\n\xED\xA0\xBC \xED\xBF\x88"}, StringView{"valid.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    REQUIRE(parser.next(context) == U'a');
    REQUIRE(parser.next(context) == U'\t');
    REQUIRE(parser.next(context) == U'\xE9');
    REQUIRE(parser.next(context) == U'\n');
    REQUIRE(parser.next(context) == 0xD83C);
    REQUIRE(parser.next(context) == U' ');
    REQUIRE(parser.next(context) == 0xDFC8);
    REQUIRE(parser.next(context) == end_of_file);
    REQUIRE(context.empty());
    REQUIRE(parser.at_eof());
}

TEST_CASE ("ParsedEnumerator next reports Unicode decoding errors with aligned carets", "[parse]")
{
    const auto test_case = GENERATE(table<StringLiteral, StringLiteral>({
        {"hello \xFF too big",
         "parse.txt:1:7: error: invalid code unit\n"
         "hello \n"
         "      ^"},
        {"a\t\x9C",
         "parse.txt:1:9: error: found continue code unit in start position\n"
         "a\t\n"
         " \t^"},
        {"ok\n\xF0",
         "parse.txt:2:1: error: found end of string in middle of code point\n"
         "\n"
         "^"},
        {"\xE6\x9C\xAC\xED\xA0\xBC\xED\xBF\x88",
         "parse.txt:1:2: error: trailing surrogate following leading surrogate (paired surrogates are invalid)\n"
         "\xE6\x9C\xAC\n"
         "  ^"},
        {"missing three one: \xE6\x9C",
         "parse.txt:1:20: error: found end of string in middle of code point\n"
         "missing three one: \n"
         "                   ^"},
    }));

    const auto& input = std::get<0>(test_case);
    const auto& expected_output = std::get<1>(test_case);

    ParsedDocument doc(input, "parse.txt");
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    for (;;)
    {
        auto ch = parser.next(context);
        REQUIRE(ch != Unicode::end_of_file);
        if (ch == Unicode::error_occurred)
        {
            break;
        }
    }

    REQUIRE(parser.at_eof());
    REQUIRE_FALSE(context.empty());
    REQUIRE(context.to_string() == expected_output);
}

TEST_CASE ("ParsedEnumerator match_while consumes matching Unicode without overconsuming", "[parse]")
{
    ParsedDocument doc(StringView{"ab\xC3\xA9!"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    const auto consumed = parser.match_while(context, [](char32_t ch) { return ch != U'!'; });

    REQUIRE(consumed.has_value());
    REQUIRE(consumed.get()->to_string() == "ab\xC3\xA9");
    REQUIRE(context.empty());
    REQUIRE(parser.next(context) == U'!');
    REQUIRE(parser.next(context) == end_of_file);
}

TEST_CASE ("ParsedEnumerator match_while returns nullopt on decode error", "[parse]")
{
    ParsedDocument doc(StringView{"ok\xFF"
                                  "bad"},
                       StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    const auto consumed = parser.match_while(context, [](char32_t) { return true; });

    REQUIRE_FALSE(consumed.has_value());
    REQUIRE(parser.at_eof());
    REQUIRE_FALSE(context.empty());
    REQUIRE(context.to_string() == "parse.txt:1:3: error: invalid code unit\n"
                                   "ok\n"
                                   "  ^");
}

TEST_CASE ("ParsedEnumerator whitespace helpers update line and column", "[parse]")
{
    {
        ParsedDocument doc(StringView{"\t \n  x"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        REQUIRE(parser.skip_whitespace().to_string() == "\t \n  ");
        REQUIRE(marker_at(parser) == "parse.txt:2:3: error: marker\n"
                                     "  x\n"
                                     "  ^");
    }

    {
        ParsedDocument doc(StringView{"\t x\n"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        REQUIRE(parser.skip_tabs_spaces().to_string() == "\t ");
        REQUIRE(marker_at(parser) == "parse.txt:1:10: error: marker\n"
                                     "\t x\n"
                                     "\t ^");
    }

    {
        ParsedDocument doc(StringView{"\r\nx"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        parser.skip_newline();
        REQUIRE(marker_at(parser) == "parse.txt:2:1: error: marker\n"
                                     "x\n"
                                     "^");
    }

    {
        ParsedDocument doc(StringView{"\r"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        parser.skip_newline();
        REQUIRE(parser.at_eof());
        REQUIRE(marker_at(parser) == "parse.txt:2:1: error: marker\n"
                                     "\n"
                                     "^");
    }

    {
        ParsedDocument doc(StringView{""}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        parser.skip_newline();
        REQUIRE(parser.at_eof());
        REQUIRE(marker_at(parser) == "parse.txt:1:1: error: marker\n"
                                     "\n"
                                     "^");
    }
}

TEST_CASE ("ParsedEnumerator ascii and unicode scanning helpers update line and column", "[parse]")
{
    {
        ParsedDocument doc(StringView{"a\t \nb\xC3\xA9!"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        REQUIRE(parser.match_while_ascii([](char) { return true; }).to_string() == "a\t \nb");
        REQUIRE(marker_at(parser) == "parse.txt:2:2: error: marker\n"
                                     "b\xC3\xA9!\n"
                                     " ^");
    }

    {
        ParsedDocument doc(StringView{"a\t\xC3\xA9\nx"}, StringView{"parse.txt"});
        FullyBufferedDiagnosticContext context;
        auto parser = doc.enumerator();

        REQUIRE(parser.skip_line(context));
        REQUIRE(context.empty());
        REQUIRE(marker_at(parser) == "parse.txt:2:1: error: marker\n"
                                     "x\n"
                                     "^");
    }

    {
        ParsedDocument doc(StringView{"ok\t\xFF"
                                      "bad"},
                           StringView{"parse.txt"});
        FullyBufferedDiagnosticContext context;
        auto parser = doc.enumerator();

        REQUIRE_FALSE(parser.skip_line(context));
        REQUIRE(parser.at_eof());
        REQUIRE(context.to_string() == "parse.txt:1:9: error: invalid code unit\n"
                                       "ok\t\n"
                                       "  \t^");
    }

    {
        ParsedDocument doc(StringView{"ab\xC3\xA9!"}, StringView{"parse.txt"});
        FullyBufferedDiagnosticContext context;
        auto parser = doc.enumerator();

        const auto consumed = parser.match_while(context, [](char32_t ch) { return ch != U'!'; });

        REQUIRE(consumed.has_value());
        REQUIRE(consumed.get()->to_string() == "ab\xC3\xA9");
        REQUIRE(context.empty());
        REQUIRE(marker_at(parser) == "parse.txt:1:4: error: marker\n"
                                     "ab\xC3\xA9!\n"
                                     "   ^");
    }
}

TEST_CASE ("ParsedEnumerator character and keyword helpers update line and column", "[parse]")
{
    {
        ParsedDocument doc(StringView{"\ta!"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();
        FullyBufferedDiagnosticContext context;

        REQUIRE(parser.skip_tabs_spaces().to_string() == "\t");
        REQUIRE(parser.require_character(context, 'a'));
        REQUIRE(context.empty());
        REQUIRE(marker_at(parser) == "parse.txt:1:10: error: marker\n"
                                     "\ta!\n"
                                     "\t ^");
    }

    {
        ParsedDocument doc(StringView{""}, StringView{"parse.txt"});
        auto parser = doc.enumerator();
        FullyBufferedDiagnosticContext context;

        REQUIRE_FALSE(parser.require_character(context, 'a'));
        REQUIRE(parser.at_eof());
        REQUIRE(context.to_string() == "parse.txt:1:1: error: expected 'a' here\n"
                                       "\n"
                                       "^");
    }

    {
        ParsedDocument doc(StringView{"\nword x"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();
        FullyBufferedDiagnosticContext context;

        parser.skip_newline();
        REQUIRE(parser.require_keyword(context, "word"));
        REQUIRE(context.empty());
        REQUIRE(marker_at(parser) == "parse.txt:2:5: error: marker\n"
                                     "word x\n"
                                     "    ^");
    }

    {
        ParsedDocument doc(StringView{"\n!a"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        parser.skip_newline();
        REQUIRE(parser.try_match_character('!'));
        REQUIRE(marker_at(parser) == "parse.txt:2:2: error: marker\n"
                                     "!a\n"
                                     " ^");
    }

    {
        ParsedDocument doc(StringView{"ab"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        REQUIRE_FALSE(parser.try_match_character('!'));
        REQUIRE(marker_at(parser) == "parse.txt:1:1: error: marker\n"
                                     "ab\n"
                                     "^");
    }

    {
        ParsedDocument doc(StringView{""}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        REQUIRE_FALSE(parser.try_match_character('!'));
        REQUIRE(parser.at_eof());
        REQUIRE(marker_at(parser) == "parse.txt:1:1: error: marker\n"
                                     "\n"
                                     "^");
    }

    {
        ParsedDocument doc(StringView{"key rest"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        REQUIRE(parser.try_match_keyword("key"));
        REQUIRE(marker_at(parser) == "parse.txt:1:4: error: marker\n"
                                     "key rest\n"
                                     "   ^");
    }

    {
        ParsedDocument doc(StringView{"key!"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();

        REQUIRE_FALSE(parser.try_match_keyword("key"));
        REQUIRE(marker_at(parser) == "parse.txt:1:1: error: marker\n"
                                     "key!\n"
                                     "^");
    }
}
