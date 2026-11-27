// Registration functions of the runtime's services, one per source file.
#pragma once

#include <cstdint>

namespace holy::xgr {

// SHA-256 of `purpose`, the host's machine id and the title id: identifiers
// that are stable per host and title and identify neither (system.cpp).
bool host_title_digest(const char* purpose, uint8_t (&digest)[32]);

// The id of the user a handle the user service gave out names; false for
// handles it did not give out (user.cpp).
bool local_user_id(void* handle, uint64_t* id);

void register_core_interface();
void register_display();
void register_game_save();
void register_local_storage();
void register_networking();
void register_package();
void register_speech();
void register_system();
void register_user();

}  // namespace holy::xgr
