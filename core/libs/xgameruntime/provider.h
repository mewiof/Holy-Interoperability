// Provider interfaces of the game runtime.
//
// Observed contract: every runtime function a title calls is a thunk in the
// title's statically linked client library. The thunk asks the runtime for an
// interface with QueryApiImpl(&provider_id, &interface_id, &object) (both ids
// have been the same GUID in every thunk observed), calls one method through
// the object's vtable, then calls Release (vtable slot 2). Slots 0..2 are the
// IUnknown methods; the runtime's functions start at slot 3.
//
// The runtime's services are process-wide singletons, so objects here are
// static and their reference counts are not tracked.
//
// Every slot of every interface starts out as a reporting stub: a call into a
// method nobody has implemented reports the interface, the slot and the
// caller in the title, then applies the unimplemented-import policy. Unknown
// interface ids get an interface made only of such stubs, so a title's request
// for a service always shows up at the call that needs it.
#pragma once

#include <windows.h>

#include <cstdint>

namespace holy::xgr {

inline constexpr int kMaxSlots = 128;

struct Interface;

// What a title holds: an object whose first word is the vtable.
struct Object {
  void* const* vtable;
  Interface* iface;
};

struct Interface {
  GUID iid{};
  const char* name = nullptr;
  void* vtable[kMaxSlots] = {};
  const char* method_names[kMaxSlots] = {};
  Object object{};
};

// Declares (or returns the already declared) interface with this id. `name`
// is a static string naming the service for logs.
Interface& define_interface(const GUID& iid, const char* name);

// Installs an implementation for a slot. `fn` must have the method's exact
// signature with the object pointer as its first parameter.
template <typename Fn>
void set_method(Interface& iface, int slot, const char* name, Fn fn) {
  iface.vtable[slot] = reinterpret_cast<void*>(fn);
  iface.method_names[slot] = name;
}

// Looks an interface up by id; unknown ids get an all-stub interface.
Interface& find_interface(const GUID& iid);

// Formats a GUID in registry form without braces.
void format_guid(const GUID& guid, char (&out)[40]);

// Registers every implemented service. Called once when the runtime starts.
void register_services();

}  // namespace holy::xgr
