// Registration functions of the runtime's services, one per source file.
#pragma once

namespace holy::xgr {

void register_core_interface();
void register_networking();
void register_system();
void register_user();

}  // namespace holy::xgr
