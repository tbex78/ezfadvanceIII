#include "ezfadvance/save_bank_selector.hpp"

#include <cassert>

int main()
{
    using ezfadvance::SaveBankSelector;

    assert(SaveBankSelector::parse("0x0900")->value() == 0x0900);
    assert(SaveBankSelector::parse("0X0910")->value() == 0x0910);
    assert(SaveBankSelector::parse("2336")->value() == 0x0920);
    assert(SaveBankSelector::parse("0x0930")->value() == 0x0930);

    assert(!SaveBankSelector::parse(""));
    assert(!SaveBankSelector::parse("0x"));
    assert(!SaveBankSelector::parse("0x08f0"));
    assert(!SaveBankSelector::parse("0x0901"));
    assert(!SaveBankSelector::parse("0x0940"));
    assert(!SaveBankSelector::parse("nonsense"));

    assert(SaveBankSelector::parse("0x0900")->accommodates(0x8000));
    assert(SaveBankSelector::parse("0x0900")->accommodates(0x10000));
    assert(SaveBankSelector::parse("0x0920")->accommodates(0x10000));
    assert(!SaveBankSelector::parse("0x0930")->accommodates(0x10000));
    assert(!SaveBankSelector::parse("0x0900")->accommodates(0));
    assert(!SaveBankSelector::parse("0x0900")->accommodates(512));
}
