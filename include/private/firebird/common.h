//
// Copyright (C) 2004-2006 Maciej Sobczak, Stephen Hutton, Rafal Bobrowski
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)
//

#ifndef SOCI_FIREBIRD_COMMON_H_INCLUDED
#define SOCI_FIREBIRD_COMMON_H_INCLUDED

#include "soci/firebird/soci-firebird.h"
#include "soci-compiler.h"
#include "soci-ssize.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>

namespace soci
{

namespace details
{

namespace firebird
{

char * allocBuffer(XSQLVAR* var);

void tmEncode(short type, std::tm * src, void * dst);

void tmDecode(short type, void * src, std::tm * dst);

void setTextParam(char const * s, std::size_t size, char * buf_,
    XSQLVAR * var);

std::string getTextParam(XSQLVAR const *var);

// Copy contents of a BLOB in buf into the given string.
void copy_from_blob(firebird_statement_backend &st, char *buf, std::string &out);

template <typename IntType>
const char *str2dec(const char * s, IntType &out, short &scale)
{
    int sign = 1;
    if ('+' == *s)
        ++s;
    else if ('-' == *s)
    {
        sign = -1;
        ++s;
    }
    scale = 0;
    bool period = false;
    IntType res = 0;
    for (out = 0; *s; ++s, out = res)
    {
        if (*s == '.')
        {
            if (period)
                return s;
            period = true;
            continue;
        }
        int d = *s - '0';
        if (d < 0 || d > 9)
            return s;
        res = res * 10 + static_cast<IntType>(d * sign);
        if (1 == sign)
        {
            if (res < out)
                return s;
        }
        else
        {
            if (res > out)
                return s;
        }
        if (period)
            ++scale;
    }
    return s;
}

template <typename T>
inline
T round_for_isc(T value)
{
  return value;
}

inline
double round_for_isc(double value)
{
  // Unfortunately all the rounding functions are C99 and so are not supported
  // by MSVC, so do it manually.
  return value < 0 ? value - 0.5 : value + 0.5;
}

//helper template to generate proper code based on compile time type check
template<bool cond> struct cond_to_isc {};
template<> struct cond_to_isc<false>
{
    static void checkInteger(short scale, short type)
    {
        if( scale >= 0 && (type == SQL_SHORT || type == SQL_LONG || type == SQL_INT64) )
            throw soci_error("Can't convert non-integral value to integral column type");
    }
};
template<> struct cond_to_isc<true>
{
    static void checkInteger(short scale,short type)
    {
        SOCI_UNUSED(scale);
        SOCI_UNUSED(type);
    }
};

template<typename T1>
void to_isc(void * val, XSQLVAR * var, short x_scale = 0)
{
    T1 value = *reinterpret_cast<T1*>(val);
    short scale = var->sqlscale + x_scale;
    short type = var->sqltype & ~1;
    long long divisor = 1, multiplier = 1;

    cond_to_isc<std::numeric_limits<T1>::is_integer>::checkInteger(scale,type);

    for (int i = 0; i > scale; --i)
        multiplier *= 10;
    for (int i = 0; i < scale; ++i)
        divisor *= 10;

    switch (type)
    {
    case SQL_SHORT:
        {
            int16_t tmp = static_cast<int16_t>(round_for_isc(value*multiplier)/divisor);
            std::memcpy(var->sqldata, &tmp, sizeof(int16_t));
        }
        break;
    case SQL_LONG:
        {
            int32_t tmp = static_cast<int32_t>(round_for_isc(value*multiplier)/divisor);
            std::memcpy(var->sqldata, &tmp, sizeof(int32_t));
        }
        break;
    case SQL_INT64:
        {
            int64_t tmp = static_cast<int64_t>(round_for_isc(value*multiplier)/divisor);
            std::memcpy(var->sqldata, &tmp, sizeof(int64_t));
        }
        break;
    case SQL_FLOAT:
        {
            float sql_value = static_cast<float>(value);
            std::memcpy(var->sqldata, &sql_value, sizeof(float));
        }
        break;
    case SQL_DOUBLE:
        {
            double sql_value = static_cast<double>(value);
            std::memcpy(var->sqldata, &sql_value, sizeof(double));
        }
        break;
    default:
        throw soci_error("Incorrect data type for numeric conversion");
    }
}

template<typename IntType, typename UIntType>
void parse_decimal(void * val, XSQLVAR * var, const char * s)
{
    short scale;
    UIntType t1;
    IntType t2;
    if (!*str2dec(s, t1, scale))
        std::memcpy(val, &t1, sizeof(t1));
    else if (!*str2dec(s, t2, scale))
        std::memcpy(val, &t2, sizeof(t2));
    else
        throw soci_error("Could not parse decimal value.");
    to_isc<IntType>(val, var, scale);
}

template<typename IntType>
std::string format_decimal(const void *sqldata, int sqlscale)
{
    IntType x = *reinterpret_cast<const IntType *>(sqldata);
    std::stringstream out;
    out << x;
    std::string r = out.str();
    if (sqlscale < 0)
    {
        if (ssize(r) - (x < 0) <= -sqlscale)
        {
            r = std::string(size_t(x < 0), '-') +
                std::string(-sqlscale - (r.size() - (x < 0)) + 1, '0') +
                r.substr(size_t(x < 0), std::string::npos);
        }
        return r.substr(0, r.size() + sqlscale) + '.' +
            r.substr(r.size() + sqlscale, std::string::npos);
    }
    return r + std::string(sqlscale, '0');
}


template<bool cond> struct cond_from_isc {};
template<> struct cond_from_isc<true> {
    static void checkInteger(short scale)
    {
        std::ostringstream msg;
        msg << "Can't convert value with scale " << -scale
            << " to integral type";
        throw soci_error(msg.str());
    }
};
template<> struct cond_from_isc<false>
{
    static void checkInteger(short scale) { SOCI_UNUSED(scale); }
};

template<typename T1>
T1 from_isc(XSQLVAR * var)
{
    short scale = var->sqlscale;
    T1 tens = 1;

    if (scale < 0)
    {
        cond_from_isc<std::numeric_limits<T1>::is_integer>::checkInteger(scale);
        for (int i = 0; i > scale; --i)
        {
            tens *= 10;
        }
    }

    SOCI_GCC_WARNING_SUPPRESS(cast-align)

    switch (var->sqltype & ~1)
    {
    case SQL_SHORT:
        return static_cast<T1>(*reinterpret_cast<int16_t*>(var->sqldata)/tens);
    case SQL_LONG:
        return static_cast<T1>(*reinterpret_cast<int32_t*>(var->sqldata)/tens);
    case SQL_INT64:
        return static_cast<T1>(*reinterpret_cast<int64_t*>(var->sqldata)/tens);
    case SQL_FLOAT:
        return static_cast<T1>(*reinterpret_cast<float*>(var->sqldata));
    case SQL_DOUBLE:
        return static_cast<T1>(*reinterpret_cast<double*>(var->sqldata));
    default:
        throw soci_error("Incorrect data type for numeric conversion");
    }

    SOCI_GCC_WARNING_RESTORE(cast-align)
}

} // namespace firebird

} // namespace details

} // namespace soci

#endif // SOCI_FIREBIRD_COMMON_H_INCLUDED
