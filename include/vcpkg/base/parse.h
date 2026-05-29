#pragma once

#include <vcpkg/base/fwd/parse.h>

#include <vcpkg/base/diagnostics.h>
#include <vcpkg/base/messages.h>
#include <vcpkg/base/optional.h>
#include <vcpkg/base/stringview.h>
#include <vcpkg/base/unicode.h>

#include <cstdint>
#include <string>

namespace vcpkg
{
    void append_caret_line(LocalizedString& res,
                           const Unicode::Utf8Decoder& it,
                           const Unicode::Utf8Decoder& start_of_line);

    struct ParseMessages
    {
        void print_errors_or_warnings() const;
        void exit_if_errors_or_warnings() const;
        bool good() const noexcept { return m_good; }
        bool any_errors() const noexcept { return m_error_count != 0; }
        size_t error_count() const noexcept { return m_error_count; }

        const std::vector<DiagnosticLine>& lines() const noexcept { return m_lines; }

        void add_line(DiagnosticLine&& line);

        LocalizedString join() const;

        bool report(DiagnosticContext& context) const&;
        bool report(DiagnosticContext& context) &&;

    private:
        std::vector<DiagnosticLine> m_lines;
        bool m_good = true;
        size_t m_error_count = 0;
    };

    struct ParserBase
    {
        ParserBase(StringView text, Optional<StringView> origin, TextRowCol init_rowcol);

        static constexpr bool is_whitespace(char32_t ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }
        static constexpr bool is_lower_alpha(char32_t ch) { return ch >= 'a' && ch <= 'z'; }
        static constexpr bool is_lower_digit(char ch) { return is_lower_alpha(ch) || is_ascii_digit(ch); }
        static constexpr bool is_upper_alpha(char32_t ch) { return ch >= 'A' && ch <= 'Z'; }
        static constexpr bool is_icase_alpha(char32_t ch) { return is_lower_alpha(ch) || is_upper_alpha(ch); }
        static constexpr bool is_ascii_digit(char32_t ch) { return ch >= '0' && ch <= '9'; }
        static constexpr bool is_lineend(char32_t ch) { return ch == '\r' || ch == '\n' || ch == Unicode::end_of_file; }
        static constexpr bool is_alphanum(char32_t ch) { return is_icase_alpha(ch) || is_ascii_digit(ch); }
        static constexpr bool is_alphadash(char32_t ch) { return is_icase_alpha(ch) || ch == '-'; }
        static constexpr bool is_alphanumdash(char32_t ch) { return is_alphanum(ch) || ch == '-'; }
        static constexpr bool is_package_name_char(char32_t ch)
        {
            return is_lower_alpha(ch) || is_ascii_digit(ch) || ch == '-';
        }
        static constexpr bool is_hex_digit_lower(char32_t ch) { return is_ascii_digit(ch) || (ch >= 'a' && ch <= 'f'); }
        static constexpr bool is_hex_digit(char32_t ch) { return is_hex_digit_lower(ch) || (ch >= 'A' && ch <= 'F'); }
        static constexpr bool is_word_char(char32_t ch) { return is_alphanum(ch) || ch == '_'; }

        StringView skip_whitespace();
        StringView skip_tabs_spaces();
        void skip_newline();
        void skip_line();

        template<class Pred>
        StringView match_while(Pred p)
        {
            const char* start = m_it.pointer_to_current();
            auto ch = cur();
            while (ch != Unicode::end_of_file && p(ch))
            {
                ch = next();
            }

            return {start, m_it.pointer_to_current()};
        }

        template<class Pred>
        StringView match_until(Pred p)
        {
            return match_while([p](char32_t ch) { return !p(ch); });
        }

        bool require_character(char ch);
        bool require_text(StringLiteral keyword);

        bool try_match_keyword(StringView keyword_content);

        StringView text() const { return m_text; }
        Unicode::Utf8Decoder it() const { return m_it; }
        char32_t cur() const { return m_it.is_eof() ? Unicode::end_of_file : *m_it; }
        SourceLoc cur_loc() const { return {m_it, m_start_of_line, m_row, m_column}; }
        TextRowCol cur_rowcol() const { return {m_row, m_column}; }
        char32_t next();
        bool at_eof() const { return m_it.is_eof(); }

        void add_error(LocalizedString&& message);
        void add_error(LocalizedString&& message, const SourceLoc& loc);

        void add_warning(LocalizedString&& message);
        void add_warning(LocalizedString&& message, const SourceLoc& loc);

        void add_note(LocalizedString&& message, const SourceLoc& loc);

        const ParseMessages& messages() const { return m_messages; }
        ParseMessages&& extract_messages() { return std::move(m_messages); }

        bool try_increment(Unicode::Utf8Decoder& encoded);

    private:
        void add_line(DiagKind kind, LocalizedString&& message, const SourceLoc& loc);

        Unicode::Utf8Decoder form_utf_decoder();

        ParseMessages m_messages;

        StringView m_text;
        Optional<StringView> m_origin;
        std::uint32_t m_row;
        std::uint32_t m_column;
        Unicode::Utf8Decoder m_it;
        Unicode::Utf8Decoder m_start_of_line;
    };

    struct ParsedDocument;

    using ParseIndex = std::uint32_t;

    struct ParsePosition
    {
        ParseIndex next_index = 0;
        ParseIndex column = 1;
        ParseIndex row = 1;
        ParseIndex row_start = 0;
    };

    // This is an "enumerator" rather than an iterator because it doesn't know what the "current" value is and
    // intentionally does not follow the iterator/sentinel protocol
    struct ParseEnumerator
    {
        bool at_eof() const noexcept;

        // consume the next character, reports an error and advances to the end of the ParsedDocument if and only if a
        // UTF-8 decoding error occurs
        char32_t next(DiagnosticContext&);
        StringView skip_whitespace() noexcept;
        StringView skip_tabs_spaces() noexcept;
        void skip_newline() noexcept;
        bool skip_line(DiagnosticContext&);

        // consume ascii characters while `p` returns true, and return the consumed characters as a StringView
        //
        // if a non-ascii character is encountered, that is interpreted as `p` returning false and will not be included
        // in the result
        //
        // (this restriction avoids needing to decode UTF-8 into the "future" or handle UTF-8 decoding errors)
        //
        // Pred is called with `char`s
        template<class Pred>
        StringView match_while_ascii(Pred p) noexcept;

        // consume characters while `p` returns true, and return the consumed characters as a StringView
        //
        // if a UTF-8 decoding error occurs, reports that error into `context`, returns nullopt, and advances to the end
        // of the ParsedDocument
        //
        // Pred is called with `char32_t`s
        template<class Pred>
        Optional<StringView> match_while(DiagnosticContext& context, Pred p);

        // ch must be in the ascii subset and may not be whitespace
        bool require_character(DiagnosticContext&, char ch);
        bool try_match_character(char ch) noexcept;

        // "keyword" must be in the ascii subset, and may not contain whitespace
        bool require_keyword(DiagnosticContext&, StringLiteral keyword);
        bool try_match_keyword(StringLiteral keyword) noexcept;

        // records an error into `context` with a subsequent line with the original text, and a subsequent ^ caret line
        // pointing to the current position
        void report_error_with_caret_line(DiagnosticContext& context, LocalizedString&& message) const;

        ParseEnumerator(const ParseEnumerator&) = default;
        ParseEnumerator& operator=(const ParseEnumerator&) = default;

    private:
        // we know this part is valid UTF-8 because we already "visited" it
        StringView get_line_prefix() const noexcept;
        // the remaining size of the current line until the first UTF decoding error
        ParseIndex get_error_line_suffix_size() const noexcept;

        friend ParsedDocument;
        ParseEnumerator(const ParsedDocument& doc) : m_doc(&doc) { }

        const ParsedDocument* m_doc;
        ParsePosition m_position;
    };

    struct ParsedDocument
    {
        ParsedDocument(StringView text, Optional<StringView> origin)
            : m_text(text.data(), text.size()), m_origin(origin)
        {
        }

        ParsedDocument(const ParsedDocument&) = delete;
        ParsedDocument& operator=(const ParsedDocument&) = delete;

        ParseEnumerator enumerator() const { return ParseEnumerator(*this); }

    private:
        friend ParseEnumerator;

        std::string m_text;
        Optional<StringView> m_origin;
    };

    constexpr uint32_t column_round_tabstop(uint32_t column) noexcept
    {
        // round to next 8-width tab stop
        return column = ((column + 7u) & ~7u) + 1u;
    }

    template<class Pred>
    StringView ParseEnumerator::match_while_ascii(Pred p) noexcept
    {
        const auto first = m_position.next_index;
        while (!at_eof())
        {
            const auto ch = m_doc->m_text[m_position.next_index];
            if ((ch & 0b1000'0000u) || !p(ch))
            {
                break;
            }

            ++m_position.next_index;
            if (ch == '\t')
            {
                m_position.column = column_round_tabstop(m_position.column);
            }
            else if (ch == '\n')
            {
                m_position.column = 1;
                ++m_position.row;
                m_position.row_start = m_position.next_index;
            }
            else
            {
                ++m_position.column;
            }
        }

        return StringView{m_doc->m_text.data() + first, m_position.next_index - first};
    }

    template<class Pred>
    Optional<StringView> ParseEnumerator::match_while(DiagnosticContext& context, Pred p)
    {
        const auto first = m_position.next_index;
        while (!at_eof())
        {
            const auto old_position = m_position;
            const auto ch = next(context);
            if (ch == Unicode::error_occurred)
            {
                return nullopt;
            }

            if (ch == Unicode::end_of_file || !p(ch))
            {
                m_position = old_position;
                break;
            }
        }

        return StringView{m_doc->m_text.data() + first, m_position.next_index - first};
    }
}
