#include <threadable-tests/doctest_include.hxx>
#include <threadable/token.hxx>

SCENARIO("token: stringify state")
{
  using namespace fho;
  using fho::slot_state;
  REQUIRE(dbg::to_str(empty) == "empty");
  REQUIRE(dbg::to_str(locked) == "locked");
  REQUIRE(dbg::to_str(ready) == "ready");
  REQUIRE(dbg::to_str(epoch) == "epoch");
  REQUIRE(dbg::to_str(tag_seq) == "tag_seq");
  REQUIRE(dbg::to_str(invalid) == "invalid");
  REQUIRE(dbg::to_str(locked | ready) == "locked|ready");
  REQUIRE(dbg::to_str(locked | empty) == "locked|empty");
  REQUIRE(dbg::to_str(locked | epoch) == "locked|epoch");
  REQUIRE(dbg::to_str(locked | tag_seq) == "locked|tag_seq");
  REQUIRE(dbg::to_str(ready | empty) == "empty|ready");
  REQUIRE(dbg::to_str(ready | epoch) == "ready|epoch");
  REQUIRE(dbg::to_str(ready | tag_seq) == "ready|tag_seq");
  REQUIRE(dbg::to_str(empty | epoch) == "empty|epoch");
  REQUIRE(dbg::to_str(empty | tag_seq) == "empty|tag_seq");
  REQUIRE(dbg::to_str(epoch | tag_seq) == "epoch|tag_seq");
  REQUIRE(dbg::to_str(locked | ready | empty) == "locked|empty|ready");
  REQUIRE(dbg::to_str(locked | ready | epoch) == "locked|ready|epoch");
  REQUIRE(dbg::to_str(locked | ready | tag_seq) == "locked|ready|tag_seq");
  REQUIRE(dbg::to_str(locked | empty | epoch) == "locked|empty|epoch");
  REQUIRE(dbg::to_str(locked | empty | tag_seq) == "locked|empty|tag_seq");
  REQUIRE(dbg::to_str(locked | epoch | tag_seq) == "locked|epoch|tag_seq");
  REQUIRE(dbg::to_str(ready | empty | epoch) == "empty|ready|epoch");
  REQUIRE(dbg::to_str(ready | empty | tag_seq) == "empty|ready|tag_seq");
  REQUIRE(dbg::to_str(ready | epoch | tag_seq) == "ready|epoch|tag_seq");
  REQUIRE(dbg::to_str(empty | epoch | tag_seq) == "empty|epoch|tag_seq");
  REQUIRE(dbg::to_str(locked | ready | empty | epoch) == "locked|empty|ready|epoch");
  REQUIRE(dbg::to_str(locked | ready | empty | tag_seq) == "locked|empty|ready|tag_seq");
  REQUIRE(dbg::to_str(locked | ready | epoch | tag_seq) == "locked|ready|epoch|tag_seq");
  REQUIRE(dbg::to_str(locked | empty | epoch | tag_seq) == "locked|empty|epoch|tag_seq");
  REQUIRE(dbg::to_str(ready | empty | epoch | tag_seq) == "empty|ready|epoch|tag_seq");
  REQUIRE(dbg::to_str(locked | ready | empty | epoch | tag_seq) ==
          "locked|empty|ready|epoch|tag_seq");
  REQUIRE(dbg::to_str(slot_state(0)) == "invalid");
}
