#include <threadable/threadable.hxx>
#include <threadable/doctest_include.hxx>

#include <iostream>

SCENARIO("threadable: temp")
{
  using namespace threadable;
  
  threadable::say_hello(std::cout, "test");
  REQUIRE(true);
}
