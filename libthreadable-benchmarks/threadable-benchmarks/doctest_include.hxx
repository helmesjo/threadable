#pragma once

#include <array>
#include <iostream> // Fix libc++ link error with doctest
#include <sstream>
#include <vector>

#include <doctest/doctest.h>

namespace doctest
{
  template<typename T, auto N>
  struct StringMaker<std::array<T, N>>
  {
    static String convert(std::array<T, N> const& in)
    {
      std::ostringstream oss;

      oss << "[";
      for (typename std::array<T, N>::const_iterator it = in.begin(); it != in.end(); ++it)
      {
        oss << *it << ", ";
      }
      oss << "]";

      return oss.str().c_str();
    }
  };

  template<typename T>
  struct StringMaker<std::vector<T>>
  {
    static String convert(std::vector<T> const& in)
    {
      std::ostringstream oss;

      oss << "[";
      for (typename std::vector<T>::const_iterator it = in.begin(); it != in.end(); ++it)
      {
        oss << *it << ", ";
      }
      oss << "]";

      return oss.str().c_str();
    }
  };
}