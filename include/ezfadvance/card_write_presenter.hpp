#pragma once

#include "ezfadvance/card_writer.hpp"

#include <iosfwd>

namespace ezfadvance {

void printCardWriteSummary(const CardWriteResult& result,
                           std::ostream& output);

} // namespace ezfadvance
