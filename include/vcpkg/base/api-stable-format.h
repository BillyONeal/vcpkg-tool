#pragma once

#include <vcpkg/base/fwd/parse.h>

#include <vcpkg/base/diagnostics.h>
#include <vcpkg/base/optional.h>
#include <vcpkg/base/stringview.h>

#include <string>

namespace vcpkg
{
    namespace details
    {
        template<class F>
        bool api_stable_format_parse_cb(void* f,
                                        DiagnosticContext& context,
                                        std::string& s,
                                        StringView replacement,
                                        const StackedParseEnumerator& position)
        {
            return (*(F*)(f))(context, s, replacement, position);
        }

        Optional<std::string> api_stable_format_impl(
            DiagnosticContext& context,
            const StackedEscapeParseDocument& fmtstr,
            bool (*cb)(void*, DiagnosticContext&, std::string&, StringView, const StackedParseEnumerator&),
            void* data);
    }

    // This function exists in order to provide an API-stable formatting function similar to `std::format()` that does
    // not depend on the feature set of fmt or the C++ standard library and thus can be contractual for user interfaces.
    template<class F>
    Optional<std::string> api_stable_format(DiagnosticContext& context,
                                            const StackedEscapeParseDocument& fmtstr,
                                            F&& handler)
    {
        return details::api_stable_format_impl(context, fmtstr, &details::api_stable_format_parse_cb<F>, &handler);
    }
}
