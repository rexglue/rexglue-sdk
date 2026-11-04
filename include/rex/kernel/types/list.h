/**
 * @file        kernel/types/list.h
 * @brief       Guest list and spin lock structures
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     The list views derive from Xenia (Ben Vanik, 2013).
 */

#pragma once

#include <type_traits>

#include <rex/assert.h>
#include <rex/kernel/heap.h>
#include <rex/types.h>

namespace rex::kernel {

struct X_LIST_ENTRY {
  be_u32 flink_ptr; /**< Next entry, or the head. */
  be_u32 blink_ptr; /**< Previous entry, or the head. */
};
REX_STATIC_ASSERT_SIZE(X_LIST_ENTRY, 8);

struct X_SINGLE_LIST_ENTRY {
  be_u32 next;
};
REX_STATIC_ASSERT_SIZE(X_SINGLE_LIST_ENTRY, 4);

struct X_SLIST_HEADER {
  X_SINGLE_LIST_ENTRY next; /**< First entry of the list, 0 when empty. */
  be_u16 depth;             /**< Number of entries in the list. */
  be_u16 sequence;          /**< Counter that changes on every push and pop. */
};
REX_STATIC_ASSERT_SIZE(X_SLIST_HEADER, 8);

struct X_KSPINLOCK {
  be_u32 owner_pcr; /**< Address of the owner's PCR, 0 when free. */
};
REX_STATIC_ASSERT_SIZE(X_KSPINLOCK, 4);

}  // namespace rex::kernel

namespace rex::kernel {

/**
 * Translates a guest list pointer to a host pointer. A translator is any type with
 * TranslateVirtual<T*>(uint32_t) and HostToGuestVirtual(void*), such as kernel::GuestAddressSpace*.
 *
 * @param ptr The guest address of the list entry.
 */
template <typename VirtualTranslator>
X_LIST_ENTRY* HostListEntry(u32 ptr, VirtualTranslator context) {
  return context->template TranslateVirtual<X_LIST_ENTRY*>(ptr);
}

template <typename VirtualTranslator>
u32 GuestListAddress(X_LIST_ENTRY* ptr, VirtualTranslator context) {
  return context->HostToGuestVirtual(ptr);
}

/**
 * Resets a list head to an empty list.
 *
 * @param context The translator, or the guest address of the list head as an unsigned integer.
 */
template <typename VirtualTranslator>
void InitializeListHead(X_LIST_ENTRY* entry, VirtualTranslator context) {
  if constexpr (std::is_unsigned_v<VirtualTranslator>) {
    entry->blink_ptr = context;
    entry->flink_ptr = context;
  } else {
    u32 orig_ptr = GuestListAddress(entry, context);
    entry->blink_ptr = orig_ptr;
    entry->flink_ptr = orig_ptr;
  }
}

/**
 * Reports whether a list has no entries.
 *
 * @return True when the list head links to itself.
 */
template <typename VirtualTranslator>
bool IsListEmpty(X_LIST_ENTRY* entry, VirtualTranslator context) {
  return HostListEntry(entry->flink_ptr, context) == entry;
}

template <typename VirtualTranslator>
void RemoveEntryList(X_LIST_ENTRY* entry, VirtualTranslator context) {
  u32 front = entry->flink_ptr;
  u32 back = entry->blink_ptr;
  HostListEntry(back, context)->flink_ptr = front;
  HostListEntry(front, context)->blink_ptr = back;
}

/**
 * Unlinks an entry from its list.
 *
 * @param entry The guest address of the list entry.
 */
template <typename VirtualTranslator>
void RemoveEntryList(u32 entry, VirtualTranslator context) {
  RemoveEntryList(HostListEntry(entry, context), context);
}

/**
 * Unlinks the first entry of a list.
 *
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
 * @param list_head_guest The guest address of the list head.
 * @param entry The guest address of the list entry.
 */
template <typename VirtualTranslator>
void InsertTailList(X_LIST_ENTRY* list_head, u32 list_head_guest, X_LIST_ENTRY* host_entry,
                    u32 entry, VirtualTranslator context) {
  u32 old_tail = list_head->blink_ptr;
  host_entry->flink_ptr = list_head_guest;
  host_entry->blink_ptr = old_tail;
  HostListEntry(old_tail, context)->flink_ptr = entry;
  list_head->blink_ptr = entry;
}

/**
 * Links an entry at the tail of a list.
 *
 * @param list_head The guest address of the list head.
 * @param entry The guest address of the list entry.
 */
template <typename VirtualTranslator>
void InsertTailList(u32 list_head, u32 entry, VirtualTranslator context) {
  InsertTailList(HostListEntry(list_head, context), list_head, HostListEntry(entry, context), entry,
                 context);
}

template <typename VirtualTranslator>
void InsertTailList(X_LIST_ENTRY* list_head, X_LIST_ENTRY* entry, VirtualTranslator context) {
  InsertTailList(list_head, GuestListAddress(list_head, context), entry,
                 GuestListAddress(entry, context), context);
}

/**
 * Links an entry at the head of a list.
 *
 * @param list_head_guest The guest address of the list head.
 * @param entry The guest address of the list entry.
 */
template <typename VirtualTranslator>
void InsertHeadList(X_LIST_ENTRY* list_head, u32 list_head_guest, X_LIST_ENTRY* host_entry,
                    u32 entry, VirtualTranslator context) {
  u32 old_list_head_flink = list_head->flink_ptr;
  host_entry->flink_ptr = old_list_head_flink;
  host_entry->blink_ptr = list_head_guest;
  HostListEntry(old_list_head_flink, context)->blink_ptr = entry;
  list_head->flink_ptr = entry;
}

/**
 * Links an entry at the head of a list.
 *
 * @param list_head The guest address of the list head.
 */
template <typename VirtualTranslator>
void InsertHeadList(u32 list_head, X_LIST_ENTRY* entry, VirtualTranslator context) {
  InsertHeadList(HostListEntry(list_head, context), list_head, entry,
                 GuestListAddress(entry, context), context);
}

template <typename VirtualTranslator>
void InsertHeadList(X_LIST_ENTRY* list_head, X_LIST_ENTRY* entry, VirtualTranslator context) {
  InsertHeadList(list_head, GuestListAddress(list_head, context), entry,
                 GuestListAddress(entry, context), context);
}

/**
 * Typed intrusive list built on X_LIST_ENTRY.
 *
 * @tparam EntryListOffset The offset of the X_LIST_ENTRY member within TObject.
 */
template <typename TObject, size_t EntryListOffset>
struct X_TYPED_LIST : public X_LIST_ENTRY {
  using ThisType = X_TYPED_LIST<TObject, EntryListOffset>;

  static X_LIST_ENTRY* ObjectListEntry(TObject* obj) {
    return reinterpret_cast<X_LIST_ENTRY*>(
        &reinterpret_cast<char*>(obj)[static_cast<ptrdiff_t>(EntryListOffset)]);
  }

  static TObject* ListEntryObject(X_LIST_ENTRY* entry) {
    return reinterpret_cast<TObject*>(
        &reinterpret_cast<char*>(entry)[-static_cast<ptrdiff_t>(EntryListOffset)]);
  }

  /**
   * Iterator over the objects of a list.
   */
  template <typename VirtualTranslator>
  struct ForwardIterator {
    VirtualTranslator vt;
    u32 current_entry;

    ForwardIterator& operator++() {
      current_entry = vt->template TranslateVirtual<X_LIST_ENTRY*>(current_entry)->flink_ptr;
      return *this;
    }

    bool operator!=(u32 other_ptr) const { return current_entry != other_ptr; }

    TObject& operator*() {
      return *ListEntryObject(vt->template TranslateVirtual<X_LIST_ENTRY*>(current_entry));
    }
  };

  /**
   * Range over the objects of a list, for use in a range for loop.
   */
  template <typename VirtualTranslator>
  struct ForwardIteratorBegin {
    VirtualTranslator vt;
    ThisType* const list_head;

    ForwardIterator<VirtualTranslator> begin() {
      return ForwardIterator<VirtualTranslator>{vt, list_head->flink_ptr};
    }

    u32 end() { return vt->HostToGuestVirtual(list_head); }
  };

  template <typename VirtualTranslator>
  ForwardIteratorBegin<VirtualTranslator> IterateForward(VirtualTranslator vt) {
    return ForwardIteratorBegin<VirtualTranslator>{vt, this};
  }

  /**
   * Resets the list to empty.
   */
  template <typename VirtualTranslator>
  void Initialize(VirtualTranslator translator) {
    InitializeListHead(this, translator);
  }

  template <typename VirtualTranslator>
  void InsertHead(TObject* entry, VirtualTranslator translator) {
    InsertHeadList(static_cast<X_LIST_ENTRY*>(this), ObjectListEntry(entry), translator);
  }

  template <typename VirtualTranslator>
  void InsertTail(TObject* entry, VirtualTranslator translator) {
    InsertTailList(this, ObjectListEntry(entry), translator);
  }

  template <typename VirtualTranslator>
  bool empty(VirtualTranslator vt) const {
    return vt->template TranslateVirtual<X_LIST_ENTRY*>(flink_ptr) == this;
  }

  /** Returns the first object of the list, which must not be empty. */
  template <typename VirtualTranslator>
  TObject* HeadObject(VirtualTranslator vt) {
    return ListEntryObject(vt->template TranslateVirtual<X_LIST_ENTRY*>(flink_ptr));
  }
};

}  // namespace rex::kernel
