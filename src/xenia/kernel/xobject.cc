/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xobject.h"

#include "xenia/base/byte_stream.h"
#include "xenia/kernel/dispatch/shell.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xsymboliclink.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

XObject::XObject(Type type)
    : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type, bool host_object)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false),
      host_object_(host_object) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  assert_true(handles_.empty());
  assert_zero(pointer_ref_count_);

  if (dispatch_object_ && kernel_state_) {
    kernel_state_->dispatch()->Destroy(dispatch_object_);
  }

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

Emulator* XObject::emulator() const { return kernel_state_->emulator_; }
KernelState* XObject::kernel_state() const { return kernel_state_; }
Memory* XObject::memory() const { return kernel_state_->memory(); }

XObject::Type XObject::type() const { return type_; }

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) ==
         X_STATUS_SUCCESS;
}

void XObject::Retain() { ++pointer_ref_count_; }

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);
  stream->Write<uint32_t>(guest_object_type_ptr_.load());

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();
  guest_object_type_ptr_.store(stream->Read<uint32_t>());

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

void XObject::SetNativeType() {
  // Use the host object type for these objects.
  if (allocated_guest_object_) {
    return;
  }

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(
      guest_object_ptr_ - sizeof(X_OBJECT_HEADER));

  guest_object_type_ptr_.store(header->object_type_ptr);
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(), xe::load_and_swap<uint32_t>(
                    memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // NetDll_WSAWaitForMultipleEvents provides timeout in form of MS.
    return (uint32_t)timeout_ticks;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

namespace {

// What a finished wait means for the caller: a boost for having been
// satisfied (or woken for an APC), a yield after a timeout.
X_STATUS FinishWait(uint32_t status, uint32_t boost_increment) {
  if (status == dispatch::kStatusTimeout) {
    xe::threading::MaybeYield();
  } else if (status < dispatch::kStatusAbandonedWait0 ||
             status == dispatch::kStatusUserApc) {
    if (XThread* thread = XThread::TryGetCurrentThread()) {
      thread->BoostOnWake(static_cast<int32_t>(boost_increment));
    }
  }
  return X_STATUS(status);
}

}  // namespace

void XObject::WriteSignalState(int32_t signal_state) {
  if (guest_object_ptr_ && HasDispatcherHeader(type_)) {
    memory()
        ->TranslateVirtual<X_DISPATCH_HEADER*>(guest_object_ptr_)
        ->signal_state = uint32_t(signal_state);
  }
}

void XObject::TouchHeader() const {
  if (guest_object_ptr_ && HasDispatcherHeader(type_)) {
    volatile uint32_t signal_state =
        memory()
            ->TranslateVirtual<X_DISPATCH_HEADER*>(guest_object_ptr_)
            ->signal_state;
    (void)signal_state;
  }
}

void XObject::BindDispatchObject(dispatch::ObjectId id) {
  dispatch_object_ = id;
  kernel_state_->dispatch()->Bind(id, this);
}

void XObject::BindDispatchEvent(bool manual_reset) {
  BindDispatchObject(kernel_state_->dispatch()->Read(
      [manual_reset](dispatch::Dispatcher& core) {
        return core.NewEvent(manual_reset, false);
      }));
}

void XObject::SetDispatchEvent() {
  if (!dispatch_object_) {
    return;
  }
  kernel_state_->dispatch()->Run([this](dispatch::Dispatcher& core) {
    return core.SetEvent(dispatch_object_);
  });
}

void XObject::ResetDispatchEvent() {
  if (!dispatch_object_) {
    return;
  }
  kernel_state_->dispatch()->Run([this](dispatch::Dispatcher& core) {
    return core.ResetEvent(dispatch_object_);
  });
}

void XObject::SyncFromGuest() {
  if (!dispatch_object_ || !guest_object_ptr_ || !HasDispatcherHeader(type_)) {
    return;
  }
  TouchHeader();
  // Under the lock, the header holds what the core last wrote there, unless
  // the guest wrote it since.
  kernel_state_->dispatch()->Run([this](dispatch::Dispatcher& core) {
    int32_t guest_state = int32_t(
        uint32_t(memory()
                     ->TranslateVirtual<X_DISPATCH_HEADER*>(guest_object_ptr_)
                     ->signal_state));
    std::optional<dispatch::ObjectInfo> info = core.Query(dispatch_object_);
    if (!info || info->signal_state == guest_state) {
      return dispatch::Effects();
    }
    return core.SetSignalState(dispatch_object_, guest_state);
  });
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode,
                       uint32_t alertable, uint64_t* opt_timeout) {
  if (!dispatch_object_) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }
  SyncFromGuest();
  dispatch::WaitRequest request;
  request.objects = {dispatch_object_};
  request.deadline = dispatch::Shell::DeadlineFromTimeout(opt_timeout);
  request.alertable = alertable != 0;
  return FinishWait(kernel_state_->dispatch()->Wait(request),
                    priority_increment());
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object,
                                uint32_t wait_reason, uint32_t processor_mode,
                                uint32_t alertable, uint64_t* opt_timeout) {
  signal_object->SyncFromGuest();
  wait_object->SyncFromGuest();
  dispatch::WaitRequest request;
  if (wait_object->dispatch_object_) {
    request.objects = {wait_object->dispatch_object_};
  }
  request.deadline = dispatch::Shell::DeadlineFromTimeout(opt_timeout);
  request.alertable = alertable != 0;
  uint32_t status = wait_object->kernel_state_->dispatch()->SignalAndWait(
      [signal_object](dispatch::Dispatcher& core, dispatch::ThreadId caller) {
        return signal_object->SignalForWait(core, caller);
      },
      request);
  return FinishWait(status, wait_object->priority_increment());
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects,
                               uint32_t wait_type, uint32_t wait_reason,
                               uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  if (!count) {
    return X_STATUS_SUCCESS;
  }
  dispatch::WaitRequest request;
  request.type =
      wait_type ? dispatch::WaitType::kAny : dispatch::WaitType::kAll;
  for (uint32_t i = 0; i < count; ++i) {
    if (!objects[i]->dispatch_object_) {
      // Nothing to wait for: as a single wait on it, it is satisfied.
      if (request.type == dispatch::WaitType::kAny) {
        return X_STATUS(i);
      }
      continue;
    }
    objects[i]->SyncFromGuest();
    request.objects.push_back(objects[i]->dispatch_object_);
  }
  request.deadline = dispatch::Shell::DeadlineFromTimeout(opt_timeout);
  request.alertable = alertable != 0;
  uint32_t status = objects[0]->kernel_state_->dispatch()->Wait(request);
  uint32_t boost_increment = 0;
  if (request.type == dispatch::WaitType::kAny) {
    uint32_t index = status & 0x3F;
    if (status < dispatch::kStatusAbandonedWait0 + count && index < count) {
      boost_increment = objects[index]->priority_increment();
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      boost_increment =
          std::max(boost_increment, objects[i]->priority_increment());
    }
  }
  return FinishWait(status, boost_increment);
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header =
      kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list.blink_ptr & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state,
                                             void* native_ptr,
                                             X_OBJECT_TYPES as_type,
                                             bool already_locked) {
  assert_not_null(native_ptr);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list.flink_ptr to a magic value. When set,
  // wait_list.blink_ptr will hold a handle to our object.
  if (!already_locked) {
    global_critical_region::mutex().lock();
  }

  XObject* result = nullptr;

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);
  X_OBJECT_TYPES type = as_type;

  if (as_type == X_OBJECT_TYPES::UndefinedObject) {
    type = header->type;
  }

  if (header->wait_list.flink_ptr == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list.blink_ptr;
    result = kernel_state->object_table()
                 ->LookupObject<XObject>(handle, true)
                 .release();
    if (result) {
      // The only place the guest hands us a raw dispatch header, so the only
      // place a direct write to it can be picked up.
      result->SyncFromGuest();
    }
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    switch (type) {
      case X_OBJECT_TYPES::EventNotificationObject:
      case X_OBJECT_TYPES::EventSynchronizationObject: {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        result = ev;
      } break;
      case X_OBJECT_TYPES::MutantObject: {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        result = mutant;
      } break;
      case X_OBJECT_TYPES::SemaphoreObject: {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        result = sem;
      } break;
      case X_OBJECT_TYPES::ProcessObject:
      case X_OBJECT_TYPES::QueueObject:
      case X_OBJECT_TYPES::ThreadObject:
      case X_OBJECT_TYPES::Spare1Object:
      case X_OBJECT_TYPES::TimerNotificationObject:
      case X_OBJECT_TYPES::TimerSynchronizationObject:
      case X_OBJECT_TYPES::ApcObject:
      case X_OBJECT_TYPES::DpcObject:
      case X_OBJECT_TYPES::DeviceQueueObject:
      case X_OBJECT_TYPES::EventPairObject:
      case X_OBJECT_TYPES::InterruptObject:
      case X_OBJECT_TYPES::ProfileObject:
      default:
        assert_always();
        result = nullptr;
    }
    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    if (result) {
      StashHandle(header, result->handle());
    }
  }

  if (!already_locked) {
    global_critical_region::mutex().unlock();
  }
  return object_ref<XObject>(result);
}

}  // namespace kernel
}  // namespace xe
