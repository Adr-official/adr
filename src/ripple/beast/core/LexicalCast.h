#ifndef BEAST_MODULE_CORE_TEXT_LEXICALCAST_H_INCLUDED
#define BEAST_MODULE_CORE_TEXT_LEXICALCAST_H_INCLUDED
#include <ripple/beast/core/Config.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
namespace beast {
namespace detail {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4800)
#pragma warning(disable: 4804)
#endif
template <class Int, class FwdIt, class Accumulator>
bool
parse_integral (Int& num, FwdIt first, FwdIt last, Accumulator accumulator)
{
    num = 0;
    if (first == last)
        return false;
    while (first != last)
    {
        auto const c = *first++;
        if (c < '0' || c > '9')
            return false;
        if (!accumulator(num, Int(c - '0')))
            return false;
    }
    return true;
}
template <class Int, class FwdIt>
bool
parse_negative_integral (Int& num, FwdIt first, FwdIt last)
{
    Int limit_value = std::numeric_limits <Int>::min() / 10;
    Int limit_digit = std::numeric_limits <Int>::min() % 10;
    if (limit_digit < 0)
        limit_digit = -limit_digit;
    return parse_integral<Int> (num, first, last,
        [limit_value, limit_digit](Int& value, Int digit)
        {
            assert ((digit >= 0) && (digit <= 9));
            if (value < limit_value || (value == limit_value && digit > limit_digit))
                return false;
            value = (value * 10) - digit;
            return true;
        });
}
template <class Int, class FwdIt>
bool
parse_positive_integral (Int& num, FwdIt first, FwdIt last)
{
    Int limit_value = std::numeric_limits <Int>::max() / 10;
    Int limit_digit = std::numeric_limits <Int>::max() % 10;
    return parse_integral<Int> (num, first, last,
        [limit_value, limit_digit](Int& value, Int digit)
        {
            assert ((digit >= 0) && (digit <= 9));
            if (value > limit_value || (value == limit_value && digit > limit_digit))
                return false;
            value = (value * 10) + digit;
            return true;
        });
}
template <class IntType, class FwdIt>
bool
parseSigned (IntType& result, FwdIt first, FwdIt last)
{
    static_assert(std::is_signed<IntType>::value,
        "You may only call parseSigned with a signed integral type.");
    if (first != last && *first == '-')
        return parse_negative_integral (result, first + 1, last);
    if (first != last && *first == '+')
        return parse_positive_integral (result, first + 1, last);
    return parse_positive_integral (result, first, last);
}
template <class UIntType, class FwdIt>
bool
parseUnsigned (UIntType& result, FwdIt first, FwdIt last)
{
    static_assert(std::is_unsigned<UIntType>::value,
        "You may only call parseUnsigned with an unsigned integral type.");
    if (first != last && *first == '+')
        return parse_positive_integral (result, first + 1, last);
    return parse_positive_integral (result, first, last);
}
template <class Out, class In>
struct LexicalCast;
template <class In>
struct LexicalCast <std::string, In>
{
    explicit LexicalCast() = default;
    template <class Arithmetic = In>
    std::enable_if_t <std::is_arithmetic <Arithmetic>::value, bool>
    operator () (std::string& out, Arithmetic in)
    {
        out = std::to_string (in);
        return true;
    }
    template <class Enumeration = In>
    std::enable_if_t <std::is_enum <Enumeration>::value, bool>
    operator () (std::string& out, Enumeration in)
    {
        out = std::to_string (
            static_cast <std::underlying_type_t <Enumeration>> (in));
        return true;
    }
};
template <class Out>
struct LexicalCast <Out, std::string>
{
    explicit LexicalCast() = default;
    static_assert (std::is_integral <Out>::value,
        "beast::LexicalCast can only be used with integral types");
    template <class Integral = Out>
    std::enable_if_t <std::is_unsigned <Integral>::value, bool>
    operator () (Integral& out, std::string const& in) const
    {
        return parseUnsigned (out, in.begin(), in.end());
    }
    template <class Integral = Out>
    std::enable_if_t <std::is_signed <Integral>::value, bool>
    operator () (Integral& out, std::string const& in) const
    {
        return parseSigned (out, in.begin(), in.end());
    }
    bool
    operator () (bool& out, std::string in) const
    {
        std::transform(in.begin (), in.end (), in.begin (),
                       [](auto c)
                       {
                           return ::tolower(static_cast<unsigned char>(c));
                       });
        if (in == "1" || in == "true")
        {
            out = true;
            return true;
        }
        if (in == "0" || in == "false")
        {
            out = false;
            return true;
        }
        return false;
    }
};
template <class Out>
struct LexicalCast <Out, char const*>
{
    explicit LexicalCast() = default;
    bool operator() (Out& out, char const* in) const
    {
        return LexicalCast <Out, std::string>()(out, in);
    }
};
template <class Out>
struct LexicalCast <Out, char*>
{
    explicit LexicalCast() = default;
    bool operator() (Out& out, char* in) const
    {
        return LexicalCast <Out, std::string>()(out, in);
    }
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
} 
struct BadLexicalCast : public std::bad_cast
{
    explicit BadLexicalCast() = default;
};
template <class Out, class In>
bool lexicalCastChecked (Out& out, In in)
{
    return detail::LexicalCast <Out, In> () (out, in);
}
template <class Out, class In>
Out lexicalCastThrow (In in)
{
    Out out;
    if (lexicalCastChecked (out, in))
        return out;
    throw BadLexicalCast ();
}
template <class Out, class In>
Out lexicalCast (In in, Out defaultValue = Out ())
{
    Out out;
    if (lexicalCastChecked (out, in))
        return out;
    return defaultValue;
}
} 
#endif
