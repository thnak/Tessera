// SPDX-License-Identifier: MIT
//
// ADR-040: composition and onSizeChange observation compile in base mode. A parent holds its
// children as developer classes (`Card`, `Watcher`), so their bind members and the parent's all
// resolve; every member of every template instantiates (explicit instantiation).

#include "Card.hpp"
#include "CardPage.hpp"
#include "WatchPage.hpp"
#include "Watcher.hpp"

namespace compose {

template struct gen::CardBase<Card>;
template struct gen::CardPageBase<CardPage>;
template struct gen::WatcherBase<Watcher>;
template struct gen::WatchPageBase<WatchPage>;

} // namespace compose
