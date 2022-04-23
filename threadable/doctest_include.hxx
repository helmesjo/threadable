#pragma once

#include <doctest/doctest.h>
#include <array>
#include <iostream> // Fix libc++ link error with doctest
#include <sstream>
#include <vector>

namespace doctest
{
    template <typename T, auto N>
    struct StringMaker<std::array<T, N> >
    {
        static String convert(const std::array<T, N>& in) {
            std::ostringstream oss;

            oss << "[";
            for(typename std::array<T, N>::const_iterator it = in.begin(); it != in.end(); ++it)
                oss << *it << ", ";
            oss << "]";

            return oss.str().c_str();
        }
    };

    template <typename T>
    struct StringMaker<std::vector<T> >
    {
        static String convert(const std::vector<T>& in) {
            std::ostringstream oss;

            oss << "[";
            for(typename std::vector<T>::const_iterator it = in.begin(); it != in.end(); ++it)
                oss << *it << ", ";
            oss << "]";

            return oss.str().c_str();
        }
    };
}