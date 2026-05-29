#include <vcpkg/base/checks.h>
#include <vcpkg/base/unicode.h>

namespace vcpkg::Unicode
{
    bool utf8_append_big_code_point(std::string& str, char32_t code_point)
    {
        if (code_point < 0x80u)
        {
            Checks::unreachable(VCPKG_LINE_INFO);
        }

        if (code_point < 0x800u)
        {
            str.push_back(static_cast<unsigned char>(0b1100'0000u | (code_point >> 6)));
            str.push_back(static_cast<unsigned char>(0b1000'0000u | (code_point & 0b0011'1111u)));
            return true;
        }

        if (code_point < 0x10000u)
        {
            // clang-format off
            str.push_back(static_cast<unsigned char>(0b1110'0000u |  (code_point  >> 12)));
            str.push_back(static_cast<unsigned char>(0b1000'0000u | ((code_point >> 6) & 0b0011'1111u)));
            str.push_back(static_cast<unsigned char>(0b1000'0000u |  (code_point        & 0b0011'1111u)));
            // clang-format on
            return true;
        }

        if (code_point < 0x110000u)
        {
            // clang-format off
            str.push_back(static_cast<unsigned char>(0b1111'0000u |  (code_point >> 18)));
            str.push_back(static_cast<unsigned char>(0b1000'0000u | ((code_point >> 12) & 0b0011'1111u)));
            str.push_back(static_cast<unsigned char>(0b1000'0000u | ((code_point >> 6)  & 0b0011'1111u)));
            str.push_back(static_cast<unsigned char>(0b1000'0000u |  (code_point        & 0b0011'1111u)));
            // clang-format on
            return true;
        }

        return false;
    }

    static utf8_errc check_trailing(unsigned char code_unit) noexcept
    {
        if ((code_unit & 0b1100'0000u) != 0b1000'0000u)
        {
            if (code_unit >= 0b1111'1000u)
            {
                return utf8_errc::InvalidCodeUnit;
            }

            return utf8_errc::UnexpectedStart;
        }

        return utf8_errc::NoError;
    }

    utf8_errc utf8_decode_code_point_slow(const char*& first, const char* last, char32_t& out) noexcept
    {
        if (first == last)
        {
            out = end_of_file;
            return utf8_errc::NoError;
        }

        auto code_unit = static_cast<unsigned char>(*first);
        if (code_unit < 0b1000'0000u)
        {
            out = code_unit;
            ++first;
            return utf8_errc::NoError;
        }

        if (code_unit < 0b1100'0000u)
        {
            out = end_of_file;
            first = last;
            return utf8_errc::UnexpectedContinue;
        }

        if (code_unit < 0b1110'0000u)
        {
            if (2 > last - first)
            {
                out = end_of_file;
                first = last;
                return utf8_errc::UnexpectedEof;
            }

            utf8_errc out_error;
            if ((out_error = check_trailing(static_cast<unsigned char>(first[1]))) != utf8_errc::NoError)
            {
                out = end_of_file;
                first = last;
                return out_error;
            }

            out = ((code_unit & 0b0001'1111) << 6) | (static_cast<unsigned char>(first[1]) & 0b0011'1111u);
            first += 2;
            return utf8_errc::NoError;
        }

        if (code_unit < 0b1111'0000u)
        {
            if (3 > last - first)
            {
                out = end_of_file;
                first = last;
                return utf8_errc::UnexpectedEof;
            }

            utf8_errc out_error;
            if ((out_error = check_trailing(static_cast<unsigned char>(first[1]))) != utf8_errc::NoError ||
                (out_error = check_trailing(static_cast<unsigned char>(first[2]))) != utf8_errc::NoError)
            {
                out = end_of_file;
                first = last;
                return out_error;
            }

            // clang-format off
            out = ((code_unit & 0b0000'1111) << 12)
                | ((static_cast<unsigned char>(first[1]) & 0b0011'1111u) << 6)
                |  (static_cast<unsigned char>(first[2]) & 0b0011'1111u);
            // clang-format on

            first += 3;
            if (utf16_is_leading_surrogate_code_point(out) && last - first >= 3)
            {
                const auto next_first = static_cast<unsigned char>(first[0]);
                const auto next_second = static_cast<unsigned char>(first[1]);
                const auto next_third = static_cast<unsigned char>(first[2]);
                if (next_first == 0xEDu && check_trailing(next_second) == utf8_errc::NoError &&
                    check_trailing(next_third) == utf8_errc::NoError)
                {
                    const char32_t next_code_point = ((next_first & 0b0000'1111u) << 12) |
                                                     ((next_second & 0b0011'1111u) << 6) | (next_third & 0b0011'1111u);
                    if (utf16_is_trailing_surrogate_code_point(next_code_point))
                    {
                        out = end_of_file;
                        first = last;
                        return utf8_errc::PairedSurrogates;
                    }
                }
            }

            return utf8_errc::NoError;
        }

        if (code_unit < 0b1111'1000u)
        {
            if (4 > last - first)
            {
                out = end_of_file;
                first = last;
                return utf8_errc::UnexpectedEof;
            }

            utf8_errc out_error;
            if ((out_error = check_trailing(static_cast<unsigned char>(first[1]))) != utf8_errc::NoError ||
                (out_error = check_trailing(static_cast<unsigned char>(first[2]))) != utf8_errc::NoError ||
                (out_error = check_trailing(static_cast<unsigned char>(first[3]))) != utf8_errc::NoError)
            {
                out = end_of_file;
                first = last;
                return out_error;
            }

            // clang-format off
            out = ((code_unit & 0b0000'0111) << 18)
                | ((static_cast<unsigned char>(first[1]) & 0b0011'1111u) << 12)
                | ((static_cast<unsigned char>(first[2]) & 0b0011'1111u) << 6)
                |  (static_cast<unsigned char>(first[3]) & 0b0011'1111u);
            // clang-format on

            if (out > 0x10'FFFF)
            {
                out = end_of_file;
                first = last;
                return utf8_errc::InvalidCodePoint;
            }

            first += 4;
            return utf8_errc::NoError;
        }

        out = end_of_file;
        first = last;
        return utf8_errc::InvalidCodeUnit;
    }

    bool utf8_is_valid_string(const char* first, const char* last) noexcept
    {
        for (;;)
        {
            if (first == last)
            {
                return true;
            }

            if (!(*first & 0b1000'0000))
            {
                // ascii fast path
                ++first;
                continue;
            }

            char32_t unused;
            if (utf8_decode_code_point(first, last, unused) != utf8_errc::NoError)
            {
                return false;
            }
        }
    }

    char32_t utf16_surrogates_to_code_point(char32_t leading, char32_t trailing) noexcept
    {
        vcpkg::Checks::check_exit(VCPKG_LINE_INFO, utf16_is_leading_surrogate_code_point(leading));
        vcpkg::Checks::check_exit(VCPKG_LINE_INFO, utf16_is_trailing_surrogate_code_point(trailing));

        char32_t res = (leading & 0b11'1111'1111) << 10;
        res |= trailing & 0b11'1111'1111;
        res += 0x0001'0000;

        return res;
    }

    LocalizedString message(utf8_errc condition)
    {
        switch (condition)
        {
            case utf8_errc::NoError: return msg::format(msgNoError);
            case utf8_errc::InvalidCodeUnit: return msg::format(msgInvalidCodeUnit);
            case utf8_errc::InvalidCodePoint: return msg::format(msgInvalidCodePoint).append_raw(" (>0x10FFFF)");
            case utf8_errc::PairedSurrogates: return msg::format(msgPairedSurrogatesAreInvalid);
            case utf8_errc::UnexpectedContinue: return msg::format(msgContinueCodeUnitInStart);
            case utf8_errc::UnexpectedStart: return msg::format(msgStartCodeUnitInContinue);
            case utf8_errc::UnexpectedEof: return msg::format(msgEndOfStringInCodeUnit);
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    utf8_errc Utf8Decoder::next() noexcept
    {
        if (is_eof())
        {
            // incremented Utf8Decoder at the end of the string
            Checks::unreachable(VCPKG_LINE_INFO);
        }

        const auto old_next = next_;
        const auto last = last_;
        if (old_next == last)
        {
            current_ = end_of_file;
            pointer_to_current_ = last;
            return utf8_errc::NoError;
        }

        char32_t code_point;
        auto err = utf8_decode_code_point(next_, last, code_point);
        if (err != utf8_errc::NoError)
        {
            current_ = end_of_file;
            pointer_to_current_ = last;
            return err;
        }

        current_ = code_point;
        pointer_to_current_ = old_next;
        return utf8_errc::NoError;
    }
}
