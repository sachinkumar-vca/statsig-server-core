#include "persistent_storage.h"
#include "libstatsig_ffi.h"
#include <array>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <utility>

using json = nlohmann::json;

namespace statsig_cpp_core {
namespace {

using CallbacksPtr = std::shared_ptr<const PersistentStorageCallbacks>;

// The C FFI hands us plain, context-free function pointers, so we cannot pass
// a capturing lambda. Instead we keep a small pool of slots, each backed by a
// distinct set of static trampoline functions (generated via templates). Each
// live PersistentStorage claims one slot; its trampolines look the callbacks
// back up by slot index. Slots hold shared_ptrs so an in-flight callback keeps
// the callbacks alive even if the owning storage is destroyed concurrently.
constexpr std::size_t kMaxSlots = 32;

std::mutex &slots_mutex() {
  static std::mutex m;
  return m;
}

std::array<CallbacksPtr, kMaxSlots> &slots() {
  static std::array<CallbacksPtr, kMaxSlots> s{};
  return s;
}

// Returns a strong reference so the callbacks outlive this dispatch even if
// the owning PersistentStorage is destroyed while the callback is in flight.
CallbacksPtr load_callbacks(std::size_t slot) {
  std::lock_guard<std::mutex> lock(slots_mutex());
  return slots()[slot];
}

void free_args(const char *args_ptr) {
  if (args_ptr != nullptr) {
    free_string(const_cast<char *>(args_ptr));
  }
}

// Takes ownership of the FFI-allocated `args_ptr` (frees it via free_string)
// and copies it out using the explicit length, so embedded or missing NULs
// cannot truncate or overread.
std::string consume_args(const char *args_ptr, uint64_t args_length) {
  if (args_ptr == nullptr) {
    return {};
  }
  std::string owned(args_ptr, static_cast<std::size_t>(args_length));
  free_args(args_ptr);
  return owned;
}

// Every dispatch_* guard swallows all exceptions: these functions are invoked
// through extern "C" function pointers from Rust, and unwinding across the FFI
// boundary is undefined behavior. The try block spans the entire dispatch —
// slot lookup (mutex lock), args copy (allocation), json parse/dump, and the
// arbitrary user callback — so no throw site sits outside the guard.
char *dispatch_load(std::size_t slot, const char *args_ptr,
                    uint64_t args_length) {
  try {
    CallbacksPtr cbs = load_callbacks(slot);
    if (cbs == nullptr || !cbs->load) {
      free_args(args_ptr);
      return nullptr;
    }

    // The args payload is the raw storage key (not JSON).
    const std::string key = consume_args(args_ptr, args_length);
    if (key.empty()) {
      return nullptr;
    }

    const std::optional<statsig_cpp_core::UserPersistedValues> result =
        cbs->load(key);
    if (!result.has_value()) {
      return nullptr;
    }

    // The Rust side copies out of this buffer immediately and does not free
    // it, so a thread_local buffer keeps the pointer valid for the duration
    // of the call without leaking. Writing it after load() returns keeps it
    // safe against reentrant loads.
    thread_local std::string buffer;
    buffer = json(result.value()).dump();
    return const_cast<char *>(buffer.c_str());
  } catch (const std::exception &e) {
    std::cerr << "[Statsig] PersistentStorage.load callback failed: "
              << e.what() << std::endl;
    return nullptr;
  } catch (...) {
    std::cerr << "[Statsig] PersistentStorage.load callback failed with a "
                 "non-std exception"
              << std::endl;
    return nullptr;
  }
}

void dispatch_save(std::size_t slot, const char *args_ptr,
                   uint64_t args_length) {
  try {
    CallbacksPtr cbs = load_callbacks(slot);
    if (cbs == nullptr || !cbs->save) {
      free_args(args_ptr);
      return;
    }

    const std::string raw = consume_args(args_ptr, args_length);
    const json args = json::parse(raw, nullptr, false);
    if (args.is_discarded() || !args.contains("key") ||
        !args["key"].is_string() || !args.contains("config_name") ||
        !args["config_name"].is_string() || !args.contains("data") ||
        !args["data"].is_object()) {
      return;
    }

    cbs->save(args["key"].get<std::string>(),
              args["config_name"].get<std::string>(),
              args["data"].get<statsig_cpp_core::StickyValues>());
  } catch (const std::exception &e) {
    std::cerr << "[Statsig] PersistentStorage.save callback failed: "
              << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Statsig] PersistentStorage.save callback failed with a "
                 "non-std exception"
              << std::endl;
  }
}

void dispatch_delete(std::size_t slot, const char *args_ptr,
                     uint64_t args_length) {
  try {
    CallbacksPtr cbs = load_callbacks(slot);
    if (cbs == nullptr || !cbs->delete_value) {
      free_args(args_ptr);
      return;
    }

    const std::string raw = consume_args(args_ptr, args_length);
    const json args = json::parse(raw, nullptr, false);
    if (args.is_discarded() || !args.contains("key") ||
        !args["key"].is_string() || !args.contains("config_name") ||
        !args["config_name"].is_string()) {
      return;
    }

    cbs->delete_value(args["key"].get<std::string>(),
                      args["config_name"].get<std::string>());
  } catch (const std::exception &e) {
    std::cerr << "[Statsig] PersistentStorage.delete callback failed: "
              << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Statsig] PersistentStorage.delete callback failed with a "
                 "non-std exception"
              << std::endl;
  }
}

template <std::size_t Slot> struct Trampoline {
  static char *load(const char *p, uint64_t len) {
    return dispatch_load(Slot, p, len);
  }
  static void save(const char *p, uint64_t len) { dispatch_save(Slot, p, len); }
  static void delete_value(const char *p, uint64_t len) {
    dispatch_delete(Slot, p, len);
  }
};

using LoadFn = char *(*)(const char *, uint64_t);
using ArgFn = void (*)(const char *, uint64_t);

template <std::size_t... I>
constexpr std::array<LoadFn, sizeof...(I)>
make_load_table(std::index_sequence<I...>) {
  return {{&Trampoline<I>::load...}};
}
template <std::size_t... I>
constexpr std::array<ArgFn, sizeof...(I)>
make_save_table(std::index_sequence<I...>) {
  return {{&Trampoline<I>::save...}};
}
template <std::size_t... I>
constexpr std::array<ArgFn, sizeof...(I)>
make_delete_table(std::index_sequence<I...>) {
  return {{&Trampoline<I>::delete_value...}};
}

const auto kLoadTable = make_load_table(std::make_index_sequence<kMaxSlots>{});
const auto kSaveTable = make_save_table(std::make_index_sequence<kMaxSlots>{});
const auto kDeleteTable =
    make_delete_table(std::make_index_sequence<kMaxSlots>{});

int claim_slot(CallbacksPtr cbs) {
  std::lock_guard<std::mutex> lock(slots_mutex());
  auto &s = slots();
  for (std::size_t i = 0; i < kMaxSlots; ++i) {
    if (s[i] == nullptr) {
      s[i] = std::move(cbs);
      return static_cast<int>(i);
    }
  }
  return -1;
}

void release_slot(int slot) {
  if (slot < 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(slots_mutex());
  slots()[static_cast<std::size_t>(slot)] = nullptr;
}

} // namespace

PersistentStorage::PersistentStorage(PersistentStorageCallbacks callbacks)
    : callbacks_(std::make_shared<const PersistentStorageCallbacks>(
          std::move(callbacks))) {
  slot_ = claim_slot(callbacks_);
  if (slot_ < 0) {
    std::cerr << "[Statsig] Exhausted PersistentStorage slots (max "
              << kMaxSlots << "); storage not registered." << std::endl;
    return;
  }

  auto s = static_cast<std::size_t>(slot_);
  ref_ = persistent_storage_create("cpp", kLoadTable[s], kSaveTable[s],
                                   kDeleteTable[s]);
  if (ref_ == 0) {
    std::cerr << "[Statsig] Failed to register persistent storage with the "
                 "native bridge."
              << std::endl;
    release_slot(slot_);
    slot_ = -1;
  }
}

PersistentStorage::~PersistentStorage() {
  if (ref_ != 0) {
    persistent_storage_release(ref_);
    ref_ = 0;
  }
  // Clear the slot after releasing the ref. Any callback still in flight holds
  // its own strong reference to the callbacks, so this cannot dangle.
  release_slot(slot_);
  slot_ = -1;
}

} // namespace statsig_cpp_core
