#pragma once

#include <iosfwd>
#include <string>

#include <threadable/export.hxx>

namespace threadable
{
  // Print a greeting for the specified name into the specified
  // stream. Throw std::invalid_argument if the name is empty.
  //
  THREADABLE_SYMEXPORT void
  say_hello (std::ostream&, const std::string& name);
}
