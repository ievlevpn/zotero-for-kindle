#include <doctest/doctest.h>

#include "platform/keyboard.h"
#include "platform/keysym.h"

using namespace zk;

TEST_CASE("keysym: latin, unicode, cyrillic and control keys") {
    CHECK(keyFromKeysym('a')->text == "a");
    CHECK(keyFromKeysym('a')->code == KeyEvent::Code::Text);
    CHECK(keyFromKeysym(' ')->text == " ");
    CHECK(keyFromKeysym(0xe9)->text == "é");
    CHECK(keyFromKeysym(0x10003b1)->text == "α");
    CHECK(keyFromKeysym(0x6c1)->text == "а");
    CHECK(keyFromKeysym(0x6e1)->text == "А");
    CHECK(keyFromKeysym(0x6c0)->text == "ю");
    CHECK(keyFromKeysym(0x6df)->text == "ъ");
    CHECK(keyFromKeysym(0x6a3)->text == "ё");
    CHECK(keyFromKeysym(0xff08)->code == KeyEvent::Code::Backspace);
    CHECK(keyFromKeysym(0xff0d)->code == KeyEvent::Code::Return);
    CHECK(keyFromKeysym(0xff8d)->code == KeyEvent::Code::Return);
    CHECK(keyFromKeysym(0xff1b)->code == KeyEvent::Code::Escape);
    CHECK_FALSE(keyFromKeysym(0xffe1));  // Shift_L
    CHECK_FALSE(keyFromKeysym(0xfe03));  // ISO_Level3_Shift
}

TEST_CASE("keyboard: lipc bounds become window coordinates") {
    Rect r = parseKeyboardBounds("0:1081:1236:567", 101);
    CHECK(r.x == 0);
    CHECK(r.y == 980);
    CHECK(r.w == 1236);
    CHECK(r.h == 567);
    CHECK(parseKeyboardBounds("", 101).w == 0);
    CHECK(parseKeyboardBounds("garbage", 101).w == 0);
}
