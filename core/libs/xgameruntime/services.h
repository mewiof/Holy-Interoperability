// Registration functions of the runtime's services, one per source file.
#pragma once

namespace holy::xgr {

void register_core_interface();
void register_display();
void register_local_storage();
void register_networking();
void register_package();
void register_speech();
void register_system();
void register_user();

}  // namespace holy::xgr
