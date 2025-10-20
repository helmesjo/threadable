#include <threadable-tests/doctest_include.hxx>
#include <threadable/token.hxx>

SCENARIO("token: stringify state")
{
  using namespace fho;
  using fho::slot_state;
  REQUIRE(dbg::to_str(empty) == "empty, 0x01");
  REQUIRE(dbg::to_str(locked) == "locked, 0x04");
  REQUIRE(dbg::to_str(ready) == "ready, 0x02");
  REQUIRE(dbg::to_str(epoch) == "epoch, 0x08");
  REQUIRE(dbg::to_str(tag_seq) == "tag_seq, 0x10");
  REQUIRE(dbg::to_str(invalid) == "invalid, 0x00");
  REQUIRE(dbg::to_str(locked | ready) == "locked|ready, 0x06");
  REQUIRE(dbg::to_str(locked | empty) == "locked|empty, 0x05");
  REQUIRE(dbg::to_str(locked | epoch) == "locked|epoch, 0x0C");
  REQUIRE(dbg::to_str(locked | tag_seq) == "locked|tag_seq, 0x14");
  REQUIRE(dbg::to_str(ready | empty) == "empty|ready, 0x03");
  REQUIRE(dbg::to_str(ready | epoch) == "ready|epoch, 0x0A");
  REQUIRE(dbg::to_str(ready | tag_seq) == "ready|tag_seq, 0x12");
  REQUIRE(dbg::to_str(empty | epoch) == "empty|epoch, 0x09");
  REQUIRE(dbg::to_str(empty | tag_seq) == "empty|tag_seq, 0x11");
  REQUIRE(dbg::to_str(epoch | tag_seq) == "epoch|tag_seq, 0x18");
  REQUIRE(dbg::to_str(locked | ready | empty) == "locked|empty|ready, 0x07");
  REQUIRE(dbg::to_str(locked | ready | epoch) == "locked|ready|epoch, 0x0E");
  REQUIRE(dbg::to_str(locked | ready | tag_seq) == "locked|ready|tag_seq, 0x16");
  REQUIRE(dbg::to_str(locked | empty | epoch) == "locked|empty|epoch, 0x0D");
  REQUIRE(dbg::to_str(locked | empty | tag_seq) == "locked|empty|tag_seq, 0x15");
  REQUIRE(dbg::to_str(locked | epoch | tag_seq) == "locked|epoch|tag_seq, 0x1C");
  REQUIRE(dbg::to_str(ready | empty | epoch) == "empty|ready|epoch, 0x0B");
  REQUIRE(dbg::to_str(ready | empty | tag_seq) == "empty|ready|tag_seq, 0x13");
  REQUIRE(dbg::to_str(ready | epoch | tag_seq) == "ready|epoch|tag_seq, 0x1A");
  REQUIRE(dbg::to_str(empty | epoch | tag_seq) == "empty|epoch|tag_seq, 0x19");
  REQUIRE(dbg::to_str(locked | ready | empty | epoch) == "locked|empty|ready|epoch, 0x0F");
  REQUIRE(dbg::to_str(locked | ready | empty | tag_seq) == "locked|empty|ready|tag_seq, 0x17");
  REQUIRE(dbg::to_str(locked | ready | epoch | tag_seq) == "locked|ready|epoch|tag_seq, 0x1E");
  REQUIRE(dbg::to_str(locked | empty | epoch | tag_seq) == "locked|empty|epoch|tag_seq, 0x1D");
  REQUIRE(dbg::to_str(ready | empty | epoch | tag_seq) == "empty|ready|epoch|tag_seq, 0x1B");
  REQUIRE(dbg::to_str(locked | ready | empty | epoch | tag_seq) ==
          "locked|empty|ready|epoch|tag_seq, 0x1F");
  REQUIRE(dbg::to_str(slot_state(0)) == "invalid, 0x00");
}
