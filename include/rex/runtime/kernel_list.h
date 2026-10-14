/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <type_traits>

#include <rex/runtime/xmemory.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/**
 * Translates a guest list pointer to a host pointer. The list helpers in this file take either a
 * translator or a raw uint32_t guest pointer where the caller has the guest address.
 *
 * The translator is any type with TranslateVirtual<T*>(uint32_t) and HostToGuestVirtual(void*)
 * methods, such as runtime::Memory*.
 *
 * @tparam VirtualTranslator The translator type.
 * @param ptr The guest address of the list entry.
 * @param context The translator.
 * @return The host list entry.
 */
template <typename VirtualTranslator>
X_LIST_ENTRY* HostList(u32 ptr, VirtualTranslator context) {
  return context->template TranslateVirtual<X_LIST_ENTRY*>(ptr);
}

/**
 * Translates a host list entry to its guest address.
 *
 * @tparam VirtualTranslator The translator type.
 * @param ptr The host list entry.
 * @param context The translator.
 * @return The guest address of the entry.
 */
template <typename VirtualTranslator>
u32 GuestList(X_LIST_ENTRY* ptr, VirtualTranslator context) {
  return context->HostToGuestVirtual(ptr);
}

/**
 * Resets a list head to an empty list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param entry The list head.
 * @param context The translator, or the guest address of the list head when an unsigned integer.
 */
template <typename VirtualTranslator>
void InitializeListHead(X_LIST_ENTRY* entry, VirtualTranslator context) {
  if constexpr (std::is_unsigned_v<VirtualTranslator>) {
    entry->blink_ptr = context;
    entry->flink_ptr = context;
  } else {
    u32 orig_ptr = GuestList(entry, context);
    entry->blink_ptr = orig_ptr;
    entry->flink_ptr = orig_ptr;
  }
}

/**
 * Reports whether a list has no entries.
 *
 * @tparam VirtualTranslator The translator type.
 * @param entry The list head.
 * @param context The translator.
 * @return True when the list head links to itself.
 */
template <typename VirtualTranslator>
bool IsListEmpty(X_LIST_ENTRY* entry, VirtualTranslator context) {
  return HostList(entry->flink_ptr, context) == entry;
}

/**
 * Unlinks an entry from its list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param entry The host list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void RemoveEntryList(X_LIST_ENTRY* entry, VirtualTranslator context) {
  u32 front = entry->flink_ptr;
  u32 back = entry->blink_ptr;
  HostList(back, context)->flink_ptr = front;
  HostList(front, context)->blink_ptr = back;
}

/**
 * Unlinks an entry from its list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param entry The guest address of the list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void RemoveEntryList(u32 entry, VirtualTranslator context) {
  RemoveEntryList(HostList(entry, context), context);
}

/**
 * Unlinks the first entry of a list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param entry The list head.
 * @param context The translator.
 * @return The guest address of the removed entry.
 */
template <typename VirtualTranslator>
u32 RemoveHeadList(X_LIST_ENTRY* entry, VirtualTranslator context) {
  u32 result = entry->flink_ptr;
  RemoveEntryList(result, context);
  return result;
}

/**
 * Links an entry at the tail of a list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param list_head The host list head.
 * @param list_head_guest The guest address of the list head.
 * @param host_entry The host list entry.
 * @param entry The guest address of the list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void InsertTailList(X_LIST_ENTRY* list_head, u32 list_head_guest, X_LIST_ENTRY* host_entry,
                    u32 entry, VirtualTranslator context) {
  u32 old_tail = list_head->blink_ptr;
  host_entry->flink_ptr = list_head_guest;
  host_entry->blink_ptr = old_tail;
  HostList(old_tail, context)->flink_ptr = entry;
  list_head->blink_ptr = entry;
}

/**
 * Links an entry at the tail of a list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param list_head The guest address of the list head.
 * @param entry The guest address of the list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void InsertTailList(u32 list_head, u32 entry, VirtualTranslator context) {
  InsertTailList(HostList(list_head, context), list_head, HostList(entry, context), entry, context);
}

/**
 * Links an entry at the tail of a list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param list_head The host list head.
 * @param entry The guest address of the list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void InsertTailList(X_LIST_ENTRY* list_head, u32 entry, VirtualTranslator context) {
  InsertTailList(list_head, GuestList(list_head, context), HostList(entry, context), entry,
                 context);
}

/**
 * Links an entry at the tail of a list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param list_head The host list head.
 * @param entry The host list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void InsertTailList(X_LIST_ENTRY* list_head, X_LIST_ENTRY* entry, VirtualTranslator context) {
  InsertTailList(list_head, GuestList(list_head, context), entry, GuestList(entry, context),
                 context);
}

/**
 * Links an entry at the head of a list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param list_head The host list head.
 * @param list_head_guest The guest address of the list head.
 * @param host_entry The host list entry.
 * @param entry The guest address of the list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void InsertHeadList(X_LIST_ENTRY* list_head, u32 list_head_guest, X_LIST_ENTRY* host_entry,
                    u32 entry, VirtualTranslator context) {
  u32 old_list_head_flink = list_head->flink_ptr;
  host_entry->flink_ptr = old_list_head_flink;
  host_entry->blink_ptr = list_head_guest;
  HostList(old_list_head_flink, context)->blink_ptr = entry;
  list_head->flink_ptr = entry;
}

/**
 * Links an entry at the head of a list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param list_head The guest address of the list head.
 * @param entry The guest address of the list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void InsertHeadList(u32 list_head, u32 entry, VirtualTranslator context) {
  InsertHeadList(HostList(list_head, context), list_head, HostList(entry, context), entry, context);
}

/**
 * Links an entry at the head of a list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param list_head The host list head.
 * @param entry The guest address of the list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void InsertHeadList(X_LIST_ENTRY* list_head, u32 entry, VirtualTranslator context) {
  InsertHeadList(list_head, GuestList(list_head, context), HostList(entry, context), entry,
                 context);
}

/**
 * Links an entry at the head of a list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param list_head The guest address of the list head.
 * @param entry The host list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void InsertHeadList(u32 list_head, X_LIST_ENTRY* entry, VirtualTranslator context) {
  InsertHeadList(HostList(list_head, context), list_head, entry, GuestList(entry, context),
                 context);
}

/**
 * Links an entry at the head of a list.
 *
 * @tparam VirtualTranslator The translator type.
 * @param list_head The host list head.
 * @param entry The host list entry.
 * @param context The translator.
 */
template <typename VirtualTranslator>
void InsertHeadList(X_LIST_ENTRY* list_head, X_LIST_ENTRY* entry, VirtualTranslator context) {
  InsertHeadList(list_head, GuestList(list_head, context), entry, GuestList(entry, context),
                 context);
}

/**
 * Typed intrusive list built on X_LIST_ENTRY.
 *
 * @tparam TObject The containing struct.
 * @tparam EntryListOffset The offsetof the X_LIST_ENTRY member within TObject.
 */
template <typename TObject, size_t EntryListOffset>
struct X_TYPED_LIST : public X_LIST_ENTRY {
  /** This list type. */
  using ThisType = X_TYPED_LIST<TObject, EntryListOffset>;

  /**
   * Returns the list entry embedded in an object.
   *
   * @param obj The object.
   * @return The list entry.
   */
  static X_LIST_ENTRY* ObjectListEntry(TObject* obj) {
    return reinterpret_cast<X_LIST_ENTRY*>(
        &reinterpret_cast<char*>(obj)[static_cast<ptrdiff_t>(EntryListOffset)]);
  }

  /**
   * Returns the object that embeds a list entry.
   *
   * @param entry The list entry.
   * @return The object.
   */
  static TObject* ListEntryObject(X_LIST_ENTRY* entry) {
    return reinterpret_cast<TObject*>(
        &reinterpret_cast<char*>(entry)[-static_cast<ptrdiff_t>(EntryListOffset)]);
  }

  /**
   * Iterator over the objects of a list.
   *
   * @tparam VirtualTranslator The translator type.
   */
  template <typename VirtualTranslator>
  struct ForwardIterator {
    VirtualTranslator vt; /**< The translator. */
    u32 current_entry;    /**< Guest address of the current entry. */

    /**
     * Advances to the next entry.
     *
     * @return This iterator.
     */
    ForwardIterator& operator++() {
      current_entry = vt->template TranslateVirtual<X_LIST_ENTRY*>(current_entry)->flink_ptr;
      return *this;
    }

    /**
     * Reports whether the iterator is at a different entry.
     *
     * @param other_ptr The guest address of the entry to compare with.
     * @return True when the entries differ.
     */
    bool operator!=(u32 other_ptr) const { return current_entry != other_ptr; }

    /**
     * Returns the object at the current entry.
     *
     * @return The object.
     */
    TObject& operator*() {
      return *ListEntryObject(vt->template TranslateVirtual<X_LIST_ENTRY*>(current_entry));
    }
  };

  /**
   * Range over the objects of a list, for use in a range for loop.
   *
   * @tparam VirtualTranslator The translator type.
   */
  template <typename VirtualTranslator>
  struct ForwardIteratorBegin {
    VirtualTranslator vt; /**< The translator. */
    ThisType* const thiz; /**< The list head. */

    /**
     * Returns an iterator at the first object.
     *
     * @return The iterator.
     */
    ForwardIterator<VirtualTranslator> begin() {
      return ForwardIterator<VirtualTranslator>{vt, thiz->flink_ptr};
    }

    /**
     * Returns the guest address of the list head, where iteration stops.
     *
     * @return The guest address.
     */
    u32 end() { return vt->HostToGuestVirtual(thiz); }
  };

  /**
   * Returns a range over the objects of the list.
   *
   * @tparam VirtualTranslator The translator type.
   * @param vt The translator.
   * @return The range.
   */
  template <typename VirtualTranslator>
  ForwardIteratorBegin<VirtualTranslator> IterateForward(VirtualTranslator vt) {
    return ForwardIteratorBegin<VirtualTranslator>{vt, this};
  }

  /**
   * Resets the list to empty.
   *
   * @tparam VirtualTranslator The translator type.
   * @param translator The translator.
   */
  template <typename VirtualTranslator>
  void Initialize(VirtualTranslator translator) {
    InitializeListHead(this, translator);
  }

  /**
   * Links an object at the head of the list.
   *
   * @tparam VirtualTranslator The translator type.
   * @param entry The object to link.
   * @param translator The translator.
   */
  template <typename VirtualTranslator>
  void InsertHead(TObject* entry, VirtualTranslator translator) {
    InsertHeadList(static_cast<X_LIST_ENTRY*>(this), ObjectListEntry(entry), translator);
  }

  /**
   * Links an object at the tail of the list.
   *
   * @tparam VirtualTranslator The translator type.
   * @param entry The object to link.
   * @param translator The translator.
   */
  template <typename VirtualTranslator>
  void InsertTail(TObject* entry, VirtualTranslator translator) {
    InsertTailList(this, ObjectListEntry(entry), translator);
  }

  /**
   * Reports whether the list has no objects.
   *
   * @tparam VirtualTranslator The translator type.
   * @param vt The translator.
   * @return True when the list is empty.
   */
  template <typename VirtualTranslator>
  bool empty(VirtualTranslator vt) const {
    return vt->template TranslateVirtual<X_LIST_ENTRY*>(flink_ptr) == this;
  }

  /**
   * Returns the first object of the list, which must not be empty.
   *
   * @tparam VirtualTranslator The translator type.
   * @param vt The translator.
   * @return The first object.
   */
  template <typename VirtualTranslator>
  TObject* HeadObject(VirtualTranslator vt) {
    return ListEntryObject(vt->template TranslateVirtual<X_LIST_ENTRY*>(flink_ptr));
  }
};

}  // namespace rex::runtime
