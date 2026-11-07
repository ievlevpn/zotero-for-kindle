#include "screens/reader_menus.h"

#include "screens/reader.h"

namespace zk {

void ReaderMenus::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Tap) reader_.closeMenus(nav);
}

}  // namespace zk
