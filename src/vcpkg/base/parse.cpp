#include <vcpkg/base/messages.h>
#include <vcpkg/base/parse.h>
#include <vcpkg/base/util.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace vcpkg
{
    static void advance_rowcol(char32_t ch, std::uint32_t& row, std::uint32_t& column)
    {
        if (row == 0)
        {
            if (column != 0)
            {
                ++column;
            }

            return;
        }

        if (ch == '\t')
        {
            column = column_round_tabstop(column);
        }
        else if (ch == '\n')
        {
            row++;
            column = 1;
        }
        else
        {
            ++column;
        }
    }

    // append whitespace intended to be printed under "matching_text" to place content "after" it on the next line
    static void append_matching_whitespace_caret(LocalizedString& target, StringView matching_text)
    {
        auto first = matching_text.begin();
        const auto last = matching_text.end();
        while (first != last)
        {
            const auto first_byte = static_cast<unsigned char>(*first);
            if (!(first_byte & 0b1000'0000))
            {
                // ascii fast path
                ++first;
                if (first_byte == '\t')
                {
                    target.append_raw('\t');
                }
                else
                {
                    target.append_raw(' ');
                }

                continue;
            }

            char32_t ch;
            if (Unicode::utf8_decode_code_point(first, last, ch) != Unicode::utf8_errc::NoError)
            {
                Checks::unreachable(VCPKG_LINE_INFO);
            }

            if (ch == '\t')
            {
                target.append_raw('\t');
            }
            else if (Unicode::is_double_width_code_point(ch))
            {
                target.append_raw(2, ' ');
            }
            else
            {
                target.append_raw(' ');
            }
        }

        target.append_raw('^');
    }

    void append_caret_line(LocalizedString& res,
                           const Unicode::Utf8Decoder& cursor,
                           const Unicode::Utf8Decoder& start_of_line)
    {
        auto line_end = cursor;
        while (!line_end.is_eof() && !ParserBase::is_lineend(*line_end))
        {
            // ignore Unicode errors just finding the line end
            (void)line_end.next();
        }

        auto print_line_before_caret =
            LocalizedString{}
                .append_indent()
                .append(msg::format(msgFormattedParseMessageExpressionPrefix))
                .append_raw(' ')
                .append_raw(StringView{start_of_line.pointer_to_current(), cursor.pointer_to_current()});

        res.append(print_line_before_caret)
            .append_raw(StringView{cursor.pointer_to_current(), line_end.pointer_to_current()})
            .append_raw('\n');

        append_matching_whitespace_caret(res, print_line_before_caret);
    }

    static void append_caret_line(LocalizedString& res, const SourceLoc& loc)
    {
        append_caret_line(res, loc.it, loc.start_of_line);
    }

    static char32_t decode_known_valid_utf8(ParseIndex& next_index, StringView text) noexcept
    {
        const auto ch = text[next_index];
        if (!(ch & 0b1000'0000))
        {
            ++next_index;
            return static_cast<unsigned char>(ch);
        }

        auto first = text.data() + next_index;
        const auto last = text.data() + text.size();
        char32_t result;
        if (Unicode::utf8_decode_code_point(first, last, result) != Unicode::utf8_errc::NoError)
        {
            Checks::unreachable(VCPKG_LINE_INFO);
        }

        next_index = static_cast<ParseIndex>(first - text.data());
        return result;
    }

    static ParseIndex get_error_line_suffix_size(StringView text, ParseIndex next_index) noexcept
    {
        const auto first = text.data() + next_index;
        const auto last = text.data() + text.size();
        auto current = first;

        while (current != last)
        {
            const auto first_byte = *current;
            if (!(first_byte & 0b1000'0000u))
            {
                if (first_byte == '\r' || first_byte == '\n')
                {
                    break;
                }

                ++current;
                continue;
            }

            char32_t ch;
            const auto decode_first = current;
            if (Unicode::utf8_decode_code_point(current, last, ch) != Unicode::utf8_errc::NoError)
            {
                current = decode_first;
                break;
            }
        }

        return static_cast<ParseIndex>(current - first);
    }

    static void report_error_with_caret_line(DiagnosticContext& context,
                                             StringView text,
                                             Optional<StringView> source_origin,
                                             const ParsePosition& position,
                                             LocalizedString&& message)
    {
        const auto line_prefix = StringView{text.data() + position.row_start, position.next_index - position.row_start};
        const auto line_suffix = get_error_line_suffix_size(text, position.next_index);
        message.append_raw('\n')
            .append_raw(StringView{line_prefix.data(), line_prefix.size() + line_suffix})
            .append_raw('\n');
        append_matching_whitespace_caret(message, line_prefix);
        if (auto origin = source_origin.get())
        {
            context.report(DiagnosticLine{
                DiagKind::Error, *origin, TextRowCol{position.row, position.column}, std::move(message)});
        }
        else
        {
            context.report(DiagnosticLine{DiagKind::Error, std::move(message)});
        }
    }

    // advances position by one code point, and updates row and column information. text[position.next_index] must be
    // valid UTF-8
    static void advance_position_known_valid(StringView text, ParsePosition& position) noexcept
    {
        const auto ch = static_cast<unsigned char>(text[position.next_index]);
        if (ch == '\t')
        {
            ++position.next_index;
            position.column = column_round_tabstop(position.column);
        }
        else if (ch == '\n')
        {
            ++position.next_index;
            position.column = 1;
            ++position.row;
            position.row_start = position.next_index;
        }
        else if (!(ch & 0b1000'0000u))
        {
            ++position.next_index;
            ++position.column;
        }
        else
        {
            ParseIndex next_index = position.next_index;
            decode_known_valid_utf8(next_index, text);
            position.next_index = next_index;
            ++position.column;
        }
    }

    void ParseMessages::print_errors_or_warnings() const
    {
        for (const auto& line : m_lines)
        {
            line.print_to(out_sink);
        }

        if (!m_good)
        {
            if (m_error_count == 0)
            {
                DiagnosticLine{DiagKind::Error, msg::format(msgWarningsTreatedAsErrors)}.print_to(out_sink);
            }

            Checks::exit_fail(VCPKG_LINE_INFO);
        }
    }

    void ParseMessages::exit_if_errors_or_warnings() const
    {
        print_errors_or_warnings();
        if (!m_good)
        {
            Checks::exit_fail(VCPKG_LINE_INFO);
        }
    }

    void ParseMessages::add_line(DiagnosticLine&& line)
    {
        switch (line.kind())
        {
            case DiagKind::Error: ++m_error_count; [[fallthrough]];
            case DiagKind::Warning: m_good = false; break;
            case DiagKind::None:
            case DiagKind::Message:
            case DiagKind::Note: break;
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }

        m_lines.push_back(std::move(line));
    }

    LocalizedString ParseMessages::join() const
    {
        std::string combined_messages;
        auto first = m_lines.begin();
        const auto last = m_lines.end();
        if (first != last)
        {
            first->to_string(combined_messages);
            while (++first != last)
            {
                combined_messages.push_back('\n');
                first->to_string(combined_messages);
            }
        }

        return LocalizedString::from_raw(std::move(combined_messages));
    }

    bool ParseMessages::report(DiagnosticContext& context) const&
    {
        for (const auto& line : m_lines)
        {
            context.report(line);
        }

        return !any_errors();
    }

    bool ParseMessages::report(DiagnosticContext& context) &&
    {
        for (auto&& line : m_lines)
        {
            context.report(std::move(line));
        }

        return !any_errors();
    }

    static Optional<StringView>& check_origin(Optional<StringView>& origin)
    {
#ifndef NDEBUG
        if (auto check_origin = origin.get())
        {
            if (check_origin->empty())
            {
                Checks::unreachable(VCPKG_LINE_INFO, "origin should not be empty");
            }
        }
#endif

        return origin;
    }

    ParserBase::ParserBase(StringView text, Optional<StringView> origin, TextRowCol init_rowcol)
        : m_messages()
        , m_text(text)
        , m_origin(check_origin(origin))
        , m_row(init_rowcol.row)
        , m_column(init_rowcol.column)
        , m_it(form_utf_decoder())
        , m_start_of_line(m_it)
    {
    }

    StringView ParserBase::skip_whitespace() { return match_while(is_whitespace); }
    StringView ParserBase::skip_tabs_spaces()
    {
        return match_while([](char32_t ch) { return ch == ' ' || ch == '\t'; });
    }

    void ParserBase::skip_newline()
    {
        if (cur() == '\r') next();
        if (cur() == '\n') next();
    }
    void ParserBase::skip_line()
    {
        match_until(is_lineend);
        skip_newline();
    }

    bool ParserBase::require_character(char ch)
    {
        if (static_cast<char32_t>(ch) == cur())
        {
            next();
            return false;
        }

        add_error(msg::format(msgExpectedCharacterHere, msg::expected = ch));
        return true;
    }

    bool ParserBase::require_text(StringLiteral text)
    {
        auto encoded = m_it;
        // check that the encoded stream matches the keyword:
        for (const char ch : text)
        {
            if (encoded.is_eof() || *encoded != static_cast<char32_t>(ch))
            {
                add_error(msg::format(msgExpectedTextHere, msg::expected = text));
                return false;
            }

            if (!try_increment(encoded))
            {
                return false;
            }
        }

        // success
        m_it = encoded;
        if (m_column != 0)
        {
            m_column += static_cast<std::uint32_t>(text.size());
        }

        return true;
    }

    bool ParserBase::try_match_keyword(StringView keyword_content)
    {
        auto encoded = m_it;
        // check that the encoded stream matches the keyword:
        for (const char ch : keyword_content)
        {
            if (encoded.is_eof() || *encoded != static_cast<char32_t>(ch))
            {
                return false;
            }

            if (!try_increment(encoded))
            {
                return false;
            }
        }

        // whole keyword matched, now check for a word boundary:
        if (!encoded.is_eof() && !is_whitespace(*encoded))
        {
            return false;
        }

        // success
        m_it = encoded;
        if (m_column != 0)
        {
            m_column += static_cast<std::uint32_t>(keyword_content.size());
        }

        return true;
    }

    char32_t ParserBase::next()
    {
        if (m_it.is_eof())
        {
            return Unicode::end_of_file;
        }
        auto ch = *m_it;
        // See https://www.gnu.org/prep/standards/standards.html#Errors
        advance_rowcol(ch, m_row, m_column);

        if (!try_increment(m_it))
        {
            return Unicode::end_of_file;
        }

        if (ch == '\n')
        {
            m_start_of_line = m_it;
        }
        if (!m_it.is_eof() && Unicode::utf16_is_surrogate_code_point(*m_it))
        {
            m_it.skip_to_eof();
        }

        return cur();
    }

    void ParserBase::add_error(LocalizedString&& message) { add_error(std::move(message), cur_loc()); }

    void ParserBase::add_error(LocalizedString&& message, const SourceLoc& loc)
    {
        // avoid cascading errors by only saving the first
        if (!m_messages.any_errors())
        {
            add_line(DiagKind::Error, std::move(message), loc);
        }

        // Avoid error loops by skipping to the end
        m_it.skip_to_eof();
    }

    void ParserBase::add_warning(LocalizedString&& message)
    {
        add_line(DiagKind::Warning, std::move(message), cur_loc());
    }

    void ParserBase::add_warning(LocalizedString&& message, const SourceLoc& loc)
    {
        add_line(DiagKind::Warning, std::move(message), loc);
    }

    void ParserBase::add_note(LocalizedString&& message, const SourceLoc& loc)
    {
        add_line(DiagKind::Note, std::move(message), loc);
    }

    bool ParserBase::try_increment(Unicode::Utf8Decoder& encoded)
    {
        auto encoding_error = encoded.next();
        if (encoding_error != Unicode::utf8_errc::NoError)
        {
            add_error(msg::format(msgUtf8ConversionFailed).append_raw(": ").append(Unicode::message(encoding_error)));
            return false;
        }

        return true;
    }

    void ParserBase::add_line(DiagKind kind, LocalizedString&& message, const SourceLoc& loc)
    {
        message.append_raw('\n');
        append_caret_line(message, loc);
        if (auto origin = m_origin.get())
        {
            m_messages.add_line(DiagnosticLine{kind, *origin, TextRowCol{loc.row, loc.column}, std::move(message)});
        }
        else
        {
            m_messages.add_line(DiagnosticLine{kind, std::move(message)});
        }
    }

    Unicode::Utf8Decoder ParserBase::form_utf_decoder()
    {
        Unicode::utf8_errc utf8_error;
        auto res = Unicode::Utf8Decoder(m_text, utf8_error);
        if (utf8_error != Unicode::utf8_errc::NoError)
        {
            // we can't use add_error because m_it and m_start_of_line aren't constructed yet
            auto message = msg::format(msgUtf8ConversionFailed).append_raw(": ").append(Unicode::message(utf8_error));
            if (auto origin = m_origin.get())
            {
                m_messages.add_line(
                    DiagnosticLine{DiagKind::Error, *origin, TextRowCol{m_row, m_column}, std::move(message)});
            }
            else
            {
                m_messages.add_line(DiagnosticLine{DiagKind::Error, std::move(message)});
            }
        }

        return res;
    }

    StackedParseEnumerator::StackedParseEnumerator(const StackedEscapeParseDocument& doc) noexcept
        : m_doc(&doc), m_decoded_next(0), m_source_next(doc.m_start_position.next_index), m_next_escape(0)
    {
    }

    bool StackedParseEnumerator::at_eof() const noexcept { return m_decoded_next == m_doc->m_decoded_text.size(); }

    char32_t StackedParseEnumerator::next() noexcept
    {
        if (at_eof())
        {
            return Unicode::end_of_file;
        }

        auto next_decoded = m_decoded_next;
        const auto result = decode_known_valid_utf8(next_decoded, m_doc->m_decoded_text);
        advance_encoded(next_decoded - m_decoded_next);
        return result;
    }

    ParsePosition StackedParseEnumerator::source_position() const noexcept
    {
        auto position = m_doc->m_start_position;

        while (position.next_index != m_source_next)
        {
            advance_position_known_valid(m_doc->m_parent_doc->m_text, position);
        }

        return position;
    }

    void StackedParseEnumerator::advance_encoded(ParseIndex count) noexcept
    {
        for (; count != 0; --count)
        {
            if (m_next_escape < m_doc->m_escape_positions.size() &&
                m_source_next == m_doc->m_escape_positions[m_next_escape])
            {
                ++m_source_next;
                ++m_next_escape;
            }

            ++m_decoded_next;
            ++m_source_next;
        }
    }

    void StackedParseEnumerator::report_error_with_caret_line(DiagnosticContext& context,
                                                              LocalizedString&& message) const
    {
        ::vcpkg::report_error_with_caret_line(
            context, m_doc->m_parent_doc->m_text, m_doc->m_parent_doc->m_origin, source_position(), std::move(message));
    }

    bool StackedParseEnumerator::require_character(DiagnosticContext& context, char ch)
    {
        if (m_decoded_next != m_doc->m_decoded_text.size() && m_doc->m_decoded_text[m_decoded_next] == ch)
        {
            advance_encoded(1);
            return true;
        }

        report_error_with_caret_line(context, msg::format(msgExpectedCharacterHere, msg::expected = ch));
        m_decoded_next = static_cast<ParseIndex>(m_doc->m_decoded_text.size());
        m_source_next = static_cast<ParseIndex>(m_doc->m_parent_doc->m_text.size());
        return false;
    }

    bool StackedParseEnumerator::try_match_character(char ch) noexcept
    {
        if (m_decoded_next != m_doc->m_decoded_text.size() && m_doc->m_decoded_text[m_decoded_next] == ch)
        {
            advance_encoded(1);
            return true;
        }

        return false;
    }

    bool StackedParseEnumerator::require_text(DiagnosticContext& context, StringLiteral text)
    {
        if (try_match_text(text))
        {
            return true;
        }

        report_error_with_caret_line(context, msg::format(msgExpectedTextHere, msg::expected = text));
        m_decoded_next = static_cast<ParseIndex>(m_doc->m_decoded_text.size());
        m_source_next = static_cast<ParseIndex>(m_doc->m_parent_doc->m_text.size());
        return false;
    }

    bool StackedParseEnumerator::try_match_text(StringLiteral text) noexcept
    {
        const auto text_size = static_cast<ParseIndex>(text.size());
        const auto remaining_size = static_cast<ParseIndex>(m_doc->m_decoded_text.size() - m_decoded_next);
        if (remaining_size >= text_size &&
            std::equal(text.begin(), text.end(), m_doc->m_decoded_text.data() + m_decoded_next))
        {
            advance_encoded(text_size);
            return true;
        }

        return false;
    }

    bool StackedParseEnumerator::require_keyword(DiagnosticContext& context, StringLiteral keyword)
    {
        if (try_match_keyword(keyword))
        {
            return true;
        }

        report_error_with_caret_line(context, msg::format(msgExpectedTextHere, msg::expected = keyword));
        m_decoded_next = static_cast<ParseIndex>(m_doc->m_decoded_text.size());
        m_source_next = static_cast<ParseIndex>(m_doc->m_parent_doc->m_text.size());
        return false;
    }

    bool StackedParseEnumerator::try_match_keyword(StringLiteral keyword) noexcept
    {
        const auto first = m_decoded_next;
        const auto keyword_size = static_cast<ParseIndex>(keyword.size());
        const auto remaining_size = static_cast<ParseIndex>(m_doc->m_decoded_text.size() - first);

        if (remaining_size < keyword_size ||
            !std::equal(keyword.begin(), keyword.end(), m_doc->m_decoded_text.data() + first) ||
            (remaining_size != keyword_size && !ParserBase::is_whitespace(m_doc->m_decoded_text[first + keyword_size])))
        {
            return false;
        }

        advance_encoded(keyword_size);
        return true;
    }

    StackedParseEnumerator StackedEscapeParseDocument::enumerator() const noexcept
    {
        return StackedParseEnumerator(*this);
    }

    ParsePosition StackedEscapeParseDocument::last_source_position() const noexcept
    {
        auto position = m_start_position;
        const auto source_end = m_start_position.next_index + static_cast<ParseIndex>(m_decoded_text.size()) +
                                static_cast<ParseIndex>(m_escape_positions.size());

        while (position.next_index != source_end)
        {
            advance_position_known_valid(m_parent_doc->m_text, position);
        }

        return position;
    }

    void StackedEscapeParseDocument::report_error_with_caret_line(DiagnosticContext& context,
                                                                  LocalizedString&& message) const
    {
        vcpkg::report_error_with_caret_line(
            context, m_parent_doc->m_text, m_parent_doc->m_origin, m_start_position, std::move(message));
    }

    void StackedEscapeParseDocument::report_error_with_caret_line_end_delimiter(DiagnosticContext& context,
                                                                                LocalizedString&& message) const
    {
        vcpkg::report_error_with_caret_line(
            context, m_parent_doc->m_text, m_parent_doc->m_origin, last_source_position(), std::move(message));
    }

    StackedEscapeParseDocument::StackedEscapeParseDocument(const ParsedDocument* parent_doc,
                                                           ParsePosition start_position,
                                                           std::string&& decoded_text,
                                                           std::vector<ParseIndex>&& escape_positions)
        : m_parent_doc(parent_doc)
        , m_start_position(start_position)
        , m_decoded_text(std::move(decoded_text))
        , m_escape_positions(std::move(escape_positions))
    {
    }

    bool ParseEnumerator::at_eof() const noexcept { return m_position.next_index == m_doc->m_text.size(); }
    char32_t ParseEnumerator::next(DiagnosticContext& context)
    {
        if (at_eof())
        {
            return Unicode::end_of_file;
        }

        const auto ch = m_doc->m_text[m_position.next_index];
        if (ch == '\t')
        {
            ++m_position.next_index;
            m_position.column = column_round_tabstop(m_position.column); // round to next 8-width tab stop
            return '\t';
        }

        if (ch == '\n')
        {
            ++m_position.next_index;
            m_position.column = 1;
            m_position.row++;
            m_position.row_start = m_position.next_index;
            return '\n';
        }

        if (!(ch & 0b1000'0000))
        {
            // ascii fast path
            ++m_position.next_index;
            ++m_position.column;
            return static_cast<char32_t>(ch);
        }

        auto first = m_doc->m_text.data() + m_position.next_index;
        const auto last = m_doc->m_text.data() + m_doc->m_text.size();
        char32_t result;
        const auto decode_error = Unicode::utf8_decode_code_point(first, last, result);
        if (decode_error == Unicode::utf8_errc::NoError)
        {
            m_position.next_index = static_cast<ParseIndex>(first - m_doc->m_text.data());
            ++m_position.column;
            return result;
        }

        report_error_with_caret_line(context, Unicode::message(decode_error));
        m_position.next_index = static_cast<ParseIndex>(m_doc->m_text.size());
        return Unicode::error_occurred;
    }

    StringView ParseEnumerator::skip_whitespace() noexcept { return match_while_ascii(ParserBase::is_whitespace); }

    StringView ParseEnumerator::skip_tabs_spaces() noexcept
    {
        return match_while_ascii([](char ch) { return ch == ' ' || ch == '\t'; });
    }

    void ParseEnumerator::skip_newline() noexcept
    {
        if (at_eof())
        {
            return;
        }

        if (m_doc->m_text[m_position.next_index] == '\r')
        {
            ++m_position.next_index;
            if (!at_eof() && m_doc->m_text[m_position.next_index] == '\n')
            {
                ++m_position.next_index;
            }

            m_position.column = 1;
            m_position.row++;
            m_position.row_start = m_position.next_index;
        }
        else if (m_doc->m_text[m_position.next_index] == '\n')
        {
            ++m_position.next_index;
            m_position.column = 1;
            m_position.row++;
            m_position.row_start = m_position.next_index;
        }
    }

    bool ParseEnumerator::skip_line(DiagnosticContext& context)
    {
        while (!at_eof())
        {
            const auto ch = m_doc->m_text[m_position.next_index];
            if (ch == '\r' || ch == '\n')
            {
                skip_newline();
                return true;
            }

            if (ch == '\t')
            {
                ++m_position.next_index;
                m_position.column = column_round_tabstop(m_position.column);
                continue;
            }

            if (!(ch & 0b1000'0000u))
            {
                // ascii fast path
                ++m_position.next_index;
                ++m_position.column;
                continue;
            }

            auto first = m_doc->m_text.data() + m_position.next_index;
            const auto last = m_doc->m_text.data() + m_doc->m_text.size();
            char32_t result;
            const auto decode_error = Unicode::utf8_decode_code_point(first, last, result);
            if (decode_error != Unicode::utf8_errc::NoError)
            {
                report_error_with_caret_line(context, Unicode::message(decode_error));
                m_position.next_index = static_cast<ParseIndex>(m_doc->m_text.size());
                return false;
            }

            m_position.next_index = static_cast<ParseIndex>(first - m_doc->m_text.data());
            ++m_position.column;
        }

        return true;
    }

    bool ParseEnumerator::require_character(DiagnosticContext& context, char ch)
    {
        if (!at_eof() && m_doc->m_text[m_position.next_index] == ch)
        {
            ++m_position.next_index;
            ++m_position.column;
            return true;
        }

        report_error_with_caret_line(context, msg::format(msgExpectedCharacterHere, msg::expected = ch));
        m_position.next_index = static_cast<ParseIndex>(m_doc->m_text.size());
        return false;
    }

    bool ParseEnumerator::try_match_character(char ch) noexcept
    {
        if (!at_eof() && m_doc->m_text[m_position.next_index] == ch)
        {
            ++m_position.next_index;
            ++m_position.column;
            return true;
        }

        return false;
    }

    bool ParseEnumerator::require_text(DiagnosticContext& context, StringLiteral text)
    {
        if (try_match_text(text))
        {
            return true;
        }

        report_error_with_caret_line(context, msg::format(msgExpectedTextHere, msg::expected = text));
        m_position.next_index = static_cast<ParseIndex>(m_doc->m_text.size());
        return false;
    }

    bool ParseEnumerator::try_match_text(StringLiteral text) noexcept
    {
        const auto text_size = static_cast<ParseIndex>(text.size());
        const auto remaining_size = static_cast<ParseIndex>(m_doc->m_text.size() - m_position.next_index);
        if (remaining_size >= text_size &&
            std::equal(text.begin(), text.end(), m_doc->m_text.data() + m_position.next_index))
        {
            m_position.next_index += text_size;
            m_position.column += text_size;
            return true;
        }

        return false;
    }

    bool ParseEnumerator::require_keyword(DiagnosticContext& context, StringLiteral keyword)
    {
        if (try_match_keyword(keyword))
        {
            return true;
        }

        report_error_with_caret_line(context, msg::format(msgExpectedTextHere, msg::expected = keyword));
        m_position.next_index = static_cast<ParseIndex>(m_doc->m_text.size());
        return false;
    }

    bool ParseEnumerator::try_match_keyword(StringLiteral keyword) noexcept
    {
        const auto keyword_size = static_cast<ParseIndex>(keyword.size());
        const auto remaining_size = static_cast<ParseIndex>(m_doc->m_text.size() - m_position.next_index);
        // check if the keyword matches and is followed by a word boundary (end of file or whitespace)
        if (remaining_size >= keyword_size &&
            std::equal(keyword.begin(), keyword.end(), m_doc->m_text.data() + m_position.next_index) &&
            (remaining_size == keyword_size ||
             ParserBase::is_whitespace(m_doc->m_text[m_position.next_index + keyword_size])))
        {
            m_position.next_index += keyword_size;
            m_position.column += keyword_size;
            return true;
        }

        return false;
    }

    Optional<StackedEscapeParseDocument> ParseEnumerator::match_escaped(DiagnosticContext& context,
                                                                        char escape_char,
                                                                        char terminal)
    {
        const char terminals[] = {terminal};
        char32_t matched_terminal;
        return match_escaped(context, matched_terminal, escape_char, StringView{terminals, 1});
    }

    Optional<StackedEscapeParseDocument> ParseEnumerator::match_escaped(DiagnosticContext& context,
                                                                        char32_t& matched_terminal,
                                                                        char escape_char,
                                                                        StringView terminals)
    {
        const auto start_position = m_position;
        std::string decoded_text;
        std::vector<ParseIndex> escape_positions;
        ParseIndex append_from = m_position.next_index;
        ParseIndex append_until = m_position.next_index;
        matched_terminal = Unicode::end_of_file;

        while (!at_eof())
        {
            if (terminals.contains(m_doc->m_text[m_position.next_index]))
            {
                append_until = m_position.next_index;
                matched_terminal = static_cast<unsigned char>(m_doc->m_text[m_position.next_index]);
                ++m_position.next_index;
                ++m_position.column;
                break;
            }

            if (m_doc->m_text[m_position.next_index] == escape_char)
            {
                decoded_text.append(m_doc->m_text.data() + append_from, m_position.next_index - append_from);
                escape_positions.push_back(m_position.next_index);
                ++m_position.next_index;
                ++m_position.column;
                if (at_eof())
                {
                    report_error_with_caret_line(context, msg::format(msgUnexpectedEOFAfterEscape));
                    return nullopt;
                }

                append_from = m_position.next_index;
            }

            if (next(context) == Unicode::error_occurred)
            {
                return nullopt;
            }

            append_until = m_position.next_index;
        }

        decoded_text.append(m_doc->m_text.data() + append_from, append_until - append_from);
        return StackedEscapeParseDocument{m_doc, start_position, std::move(decoded_text), std::move(escape_positions)};
    }

    void ParseEnumerator::report_error_with_caret_line(DiagnosticContext& context, LocalizedString&& message) const
    {
        const auto line_prefix = get_line_prefix();
        const auto line_suffix = get_error_line_suffix_size();
        message.append_raw('\n')
            .append_raw(StringView{line_prefix.data(), line_prefix.size() + line_suffix})
            .append_raw('\n');
        append_matching_whitespace_caret(message, line_prefix);
        if (auto origin = m_doc->m_origin.get())
        {
            context.report(DiagnosticLine{
                DiagKind::Error, *origin, TextRowCol{m_position.row, m_position.column}, std::move(message)});
        }
        else
        {
            context.report(DiagnosticLine{DiagKind::Error, std::move(message)});
        }
    }

    StringView ParseEnumerator::get_line_prefix() const noexcept
    {
        return StringView{m_doc->m_text.data() + m_position.row_start, m_position.next_index - m_position.row_start};
    }

    ParseIndex ParseEnumerator::get_error_line_suffix_size() const noexcept
    {
        const auto first = m_doc->m_text.data() + m_position.next_index;
        const auto last = m_doc->m_text.data() + m_doc->m_text.size();
        auto current = first;

        while (current != last)
        {
            const auto first_byte = *current;
            if (!(first_byte & 0b1000'0000u))
            {
                if (first_byte == '\r' || first_byte == '\n')
                {
                    break;
                }

                ++current;
                continue;
            }

            char32_t ch;
            const auto decode_first = current;
            if (Unicode::utf8_decode_code_point(current, last, ch) != Unicode::utf8_errc::NoError)
            {
                current = decode_first;
                break;
            }
        }

        return static_cast<ParseIndex>(current - first);
    }

    ParseEnumerator::ParseEnumerator(const ParsedDocument& doc) : m_doc(&doc) { }

    ParsedDocument::ParsedDocument(StringView text, Optional<StringView> origin)
        : m_text(text.data(), text.size()), m_origin(origin)
    {
    }

    ParseEnumerator ParsedDocument::enumerator() const { return ParseEnumerator(*this); }

    Optional<StackedEscapeParseDocument> ParsedDocument::stacked(DiagnosticContext& context) const
    {
        auto parser = enumerator();
        for (;;)
        {
            const auto ch = parser.next(context);
            if (ch == Unicode::error_occurred)
            {
                return nullopt;
            }

            if (ch == Unicode::end_of_file)
            {
                break;
            }
        }

        return StackedEscapeParseDocument{
            this, ParsePosition{0, 1, 1, 0}, std::string(m_text), std::vector<ParseIndex>{}};
    }
}
