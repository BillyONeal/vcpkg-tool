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

static std::string stacked_require_character_error_at(StackedParseEnumerator& parser, char ch)
{
    FullyBufferedDiagnosticContext context;
    REQUIRE_FALSE(parser.require_character(context, ch));
    return context.to_string();
}

static std::string stacked_require_text_error_at(StackedParseEnumerator& parser, StringLiteral text)
{
    FullyBufferedDiagnosticContext context;
    REQUIRE_FALSE(parser.require_text(context, text));
    return context.to_string();
}

static std::string stacked_marker_last_at(const StackedEscapeParseDocument& doc)
{
    FullyBufferedDiagnosticContext context;
    doc.report_error_with_caret_line_end_delimiter(context, LocalizedString::from_raw("marker"));
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

    {
        ParsedDocument doc(StringView{"key!"}, StringView{"parse.txt"});
        auto parser = doc.enumerator();
        FullyBufferedDiagnosticContext context;

        REQUIRE(parser.require_text(context, "key"));
        REQUIRE(context.empty());
        REQUIRE(marker_at(parser) == "parse.txt:1:4: error: marker\n"
                                     "key!\n"
                                     "   ^");
    }
}

TEST_CASE ("ParseEnumerator match_escaped returns stacked enumerator over decoded text", "[parse]")
{
    ParsedDocument doc(StringView{"alpha` beta`! rest!tail"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

    REQUIRE(maybe_stacked_doc.has_value());
    REQUIRE(context.empty());
    auto stacked = maybe_stacked_doc.get()->enumerator();

    REQUIRE(stacked.try_match_keyword("alpha"));
    REQUIRE(stacked.try_match_character(' '));
    REQUIRE(stacked.try_match_keyword("beta!"));
    REQUIRE(stacked.try_match_character(' '));
    REQUIRE(stacked.try_match_keyword("rest"));
    REQUIRE(context.empty());
    REQUIRE(stacked.at_eof());
    REQUIRE(parser.next(context) == U't');
    REQUIRE(context.empty());
}

TEST_CASE ("StackedParseEnumerator text helpers match decoded text", "[parse]")
{
    ParsedDocument doc(StringView{"alpha`!omegaX!tail"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

    REQUIRE(maybe_stacked_doc.has_value());
    REQUIRE(context.empty());
    auto stacked = maybe_stacked_doc.get()->enumerator();

    REQUIRE(stacked.try_match_text("alpha!"));
    REQUIRE_FALSE(stacked.try_match_keyword("omega"));
    REQUIRE(stacked.require_text(context, "omega"));
    REQUIRE(stacked.try_match_character('X'));
    REQUIRE(context.empty());
    REQUIRE(stacked.at_eof());
    REQUIRE(parser.next(context) == U't');
    REQUIRE(context.empty());
}

TEST_CASE ("StackedParseEnumerator advance paths cover both overloads", "[parse]")
{
    ParsedDocument doc(StringView{"a`b`cd`ef"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

    REQUIRE(maybe_stacked_doc.has_value());
    REQUIRE(context.empty());
    auto stacked = maybe_stacked_doc.get()->enumerator();

    REQUIRE(stacked.try_match_text("abcd"));
    REQUIRE(stacked.try_match_character('e'));
    REQUIRE(stacked.try_match_character('f'));
    REQUIRE(stacked.at_eof());
}

TEST_CASE ("ParseEnumerator match_escaped accepts eof as terminal", "[parse]")
{
    ParsedDocument doc(StringView{"key value"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

    REQUIRE(maybe_stacked_doc.has_value());
    REQUIRE(context.empty());
    REQUIRE(parser.at_eof());
    auto stacked = maybe_stacked_doc.get()->enumerator();

    REQUIRE(stacked.try_match_keyword("key"));
    REQUIRE(stacked.try_match_character(' '));
    REQUIRE(stacked.try_match_keyword("value"));
    REQUIRE(stacked.at_eof());
}

TEST_CASE ("ParseEnumerator match_escaped reports matched terminal", "[parse]")
{
    ParsedDocument doc(StringView{"alpha`!beta;tail"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();
    char32_t matched_terminal = Unicode::end_of_file;

    auto maybe_match = parser.match_escaped(context, matched_terminal, '`', "!;");

    REQUIRE(maybe_match.has_value());
    REQUIRE(context.empty());
    REQUIRE(matched_terminal == U';');

    auto stacked = maybe_match.get()->enumerator();
    REQUIRE(stacked.try_match_text("alpha!beta"));
    REQUIRE(stacked.at_eof());
    REQUIRE(parser.next(context) == U't');
    REQUIRE(context.empty());
}

TEST_CASE ("ParseEnumerator match_escaped reports no terminal at eof", "[parse]")
{
    ParsedDocument doc(StringView{"alpha`!beta"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();
    char32_t matched_terminal = U'!';

    auto maybe_match = parser.match_escaped(context, matched_terminal, '`', "!;");

    REQUIRE(maybe_match.has_value());
    REQUIRE(context.empty());
    REQUIRE(matched_terminal == Unicode::end_of_file);
    REQUIRE(parser.at_eof());

    auto stacked = maybe_match.get()->enumerator();
    REQUIRE(stacked.try_match_text("alpha!beta"));
    REQUIRE(stacked.at_eof());
}

TEST_CASE ("StackedParseEnumerator reports source positions through escapes and Unicode", "[parse]")
{
    ParsedDocument doc(StringView{"one` two\n\xC3\xA9 three"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

    REQUIRE(maybe_stacked_doc.has_value());
    REQUIRE(context.empty());
    auto stacked = maybe_stacked_doc.get()->enumerator();

    REQUIRE(stacked.try_match_keyword("one"));
    REQUIRE(stacked.try_match_character(' '));
    REQUIRE(stacked.try_match_keyword("two"));
    REQUIRE(stacked.next() == U'\n');
    REQUIRE(stacked.next() == U'\xE9');
    REQUIRE(stacked_require_character_error_at(stacked, '!') == "parse.txt:2:2: error: expected '!' here\n"
                                                                "\xC3\xA9 three\n"
                                                                " ^");
}

TEST_CASE ("StackedParseEnumerator reports source positions after escapes", "[parse]")
{
    ParsedDocument doc(StringView{"one` two three"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

    REQUIRE(maybe_stacked_doc.has_value());
    REQUIRE(context.empty());
    auto stacked = maybe_stacked_doc.get()->enumerator();

    REQUIRE(stacked.try_match_keyword("one"));
    REQUIRE(stacked.try_match_character(' '));
    REQUIRE(stacked.try_match_keyword("two"));
    REQUIRE(stacked_require_character_error_at(stacked, '!') == "parse.txt:1:9: error: expected '!' here\n"
                                                                "one` two three\n"
                                                                "        ^");
}

TEST_CASE ("StackedParseEnumerator require_text reports source positions after escapes", "[parse]")
{
    ParsedDocument doc(StringView{"one` two three"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

    REQUIRE(maybe_stacked_doc.has_value());
    REQUIRE(context.empty());
    auto stacked = maybe_stacked_doc.get()->enumerator();

    REQUIRE(stacked.try_match_text("one two"));
    REQUIRE(stacked_require_text_error_at(stacked, "!") == "parse.txt:1:9: error: expected '!' here\n"
                                                           "one` two three\n"
                                                           "        ^");
}

TEST_CASE ("StackedEscapeParseDocument report_error_with_caret_line_last points at EOF when the match ends at EOF",
           "[parse]")
{
    {
        ParsedDocument doc(StringView{"one` two three"}, StringView{"parse.txt"});
        FullyBufferedDiagnosticContext context;
        auto parser = doc.enumerator();

        auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

        const auto* stacked_doc = maybe_stacked_doc.get();
        REQUIRE(stacked_doc);
        REQUIRE(context.empty());
        REQUIRE(stacked_marker_last_at(*stacked_doc) == "parse.txt:1:15: error: marker\n"
                                                        "one` two three\n"
                                                        "              ^");
    }

    {
        ParsedDocument doc(StringView{"one` two\n\xC3\xA9 three"}, StringView{"parse.txt"});
        FullyBufferedDiagnosticContext context;
        auto parser = doc.enumerator();

        auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

        const auto* stacked_doc = maybe_stacked_doc.get();
        REQUIRE(stacked_doc);
        REQUIRE(context.empty());
        REQUIRE(stacked_marker_last_at(*stacked_doc) == "parse.txt:2:8: error: marker\n"
                                                        "\xC3\xA9 three\n"
                                                        "       ^");
    }
}

TEST_CASE ("StackedEscapeParseDocument report_error_with_caret_line_last counts source bytes not code points",
           "[parse]")
{
    // Regression test: this caret position is wrong if source advancement mistakenly treats a byte count as a
    // code-point count.
    ParsedDocument doc(StringView{"one` two\n\xC3\xA9\xC3\xA9"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

    const auto* stacked_doc = maybe_stacked_doc.get();
    REQUIRE(stacked_doc);
    REQUIRE(context.empty());
    REQUIRE(stacked_marker_last_at(*stacked_doc) == "parse.txt:2:3: error: marker\n"
                                                    "\xC3\xA9\xC3\xA9\n"
                                                    "  ^");
}

TEST_CASE ("StackedEscapeParseDocument report_error_with_caret_line_last points at the consumed end delimiter",
           "[parse]")
{
    ParsedDocument doc(StringView{"readwrite,extra"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    auto maybe_stacked_doc = parser.match_escaped(context, '`', ',');

    const auto* stacked_doc = maybe_stacked_doc.get();
    REQUIRE(stacked_doc);
    REQUIRE(context.empty());
    REQUIRE(stacked_marker_last_at(*stacked_doc) == "parse.txt:1:10: error: marker\n"
                                                    "readwrite,extra\n"
                                                    "         ^");
}

TEST_CASE ("ParseEnumerator match_escaped reports invalid UTF-8 while proving decoded text", "[parse]")
{
    ParsedDocument doc(StringView{"ok`! \xFF"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;
    auto parser = doc.enumerator();

    auto maybe_stacked_doc = parser.match_escaped(context, '`', '!');

    REQUIRE_FALSE(maybe_stacked_doc.has_value());
    REQUIRE(parser.at_eof());
    REQUIRE(context.to_string() == "parse.txt:1:6: error: invalid code unit\n"
                                   "ok`! \n"
                                   "     ^");
}

TEST_CASE ("ParsedDocument stacked returns whole input as a stacked document", "[parse]")
{
    ParsedDocument doc(StringView{"one\n\xC3\xA9 two"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;

    auto maybe_stacked_doc = doc.stacked(context);

    const auto* stacked_doc = maybe_stacked_doc.get();
    REQUIRE(stacked_doc);
    REQUIRE(context.empty());
    REQUIRE(stacked_doc->text() == "one\n\xC3\xA9 two");
    REQUIRE(stacked_marker_last_at(*stacked_doc) == "parse.txt:2:6: error: marker\n"
                                                    "\xC3\xA9 two\n"
                                                    "     ^");
}

TEST_CASE ("ParsedDocument stacked reports invalid UTF-8", "[parse]")
{
    ParsedDocument doc(StringView{"ok \xFF"}, StringView{"parse.txt"});
    FullyBufferedDiagnosticContext context;

    auto maybe_stacked_doc = doc.stacked(context);

    REQUIRE_FALSE(maybe_stacked_doc.has_value());
    REQUIRE(context.to_string() == "parse.txt:1:4: error: invalid code unit\n"
                                   "ok \n"
                                   "   ^");
}
