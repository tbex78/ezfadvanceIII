#include "ezfadvance/save_catalog_presenter.hpp"

#include <cassert>
#include <sstream>
#include <string>

int main()
{
    std::ostringstream output;
    ezfadvance::SaveCatalogPresenter presenter(output);
    presenter.showSummary(false, 2, 0x005c2360);

    ezfadvance::SaveCatalogAnalysis analysis;
    analysis.mapping_prepared = true;
    ezfadvance::SaveCatalogRom rom;
    rom.catalog_entry.name = "ABCD menu name";
    rom.catalog_entry.start = 0x00800000;
    rom.catalog_entry.type = 3;
    rom.catalog_entry.mapping = 4;
    rom.header.game_code = "ABCD";
    rom.allocation_span = 0x00400000;
    rom.save_marker = "EEPROM_V124";
    analysis.roms.push_back(rom);
    presenter.showRoms(analysis);

    const auto text = output.str();
    assert(text.find("Layout       : multi ROM") != std::string::npos);
    assert(text.find("ROM count    : 2") != std::string::npos);
    assert(text.find("Loader/menu  : 0x005c2360") != std::string::npos);
    assert(text.find("Game code    : ABCD") != std::string::npos);
    assert(text.find("Save marker  : EEPROM_V124") != std::string::npos);

    auto progress = presenter.scanProgress();
    progress(0, 2, 1, 4, false);
    progress(0, 2, 4, 4, true);
    const auto progress_text = output.str();
    assert(progress_text.find("Scanning save metadata for ROM 1/2:  25%") !=
           std::string::npos);
    assert(progress_text.find("Scanning save metadata for ROM 1/2: 100%\n") !=
           std::string::npos);
}
