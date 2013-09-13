/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Barrier_h
#define gc_Barrier_h

#include "NamespaceImports.h"

#include "gc/Heap.h"
#include "gc/StoreBuffer.h"
#include "js/HashTable.h"
#include "js/Id.h"
#include "js/RootingAPI.h"

/*
 * A write barrier is a mechanism used by incremental or generation GCs to
 * ensure that every value that needs to be marked is marked. In general, the
 * write barrier should be invoked whenever a write can cause the set of things
 * traced through by the GC to change. This includes:
 *   - writes to object properties
 *   - writes to array slots
 *   - writes to fields like JSObject::shape_ that we trace through
 *   - writes to fields in private data, like JSGenerator::obj
 *   - writes to non-markable fields like JSObject::private that point to
 *     markable data
 * The last category is the trickiest. Even though the private pointers does not
 * point to a GC thing, changing the private pointer may change the set of
 * objects that are traced by the GC. Therefore it needs a write barrier.
 *
 * Every barriered write should have the following form:
 *   <pre-barrier>
 *   obj->field = value; // do the actual write
 *   <post-barrier>
 * The pre-barrier is used for incremental GC and the post-barrier is for
 * generational GC.
 *
 *                               PRE-BARRIER
 *
 * To understand the pre-barrier, let's consider how incremental GC works. The
 * GC itself is divided into "slices". Between each slice, JS code is allowed to
 * run. Each slice should be short so that the user doesn't notice the
 * interruptions. In our GC, the structure of the slices is as follows:
 *
 * 1. ... JS work, which leads to a request to do GC ...
 * 2. [first GC slice, which performs all root marking and possibly more marking]
 * 3. ... more JS work is allowed to run ...
 * 4. [GC mark slice, which runs entirely in drainMarkStack]
 * 5. ... more JS work ...
 * 6. [GC mark slice, which runs entirely in drainMarkStack]
 * 7. ... more JS work ...
 * 8. [GC marking finishes; sweeping done non-incrementally; GC is done]
 * 9. ... JS continues uninterrupted now that GC is finishes ...
 *
 * Of course, there may be a different number of slices depending on how much
 * marking is to be done.
 *
 * The danger inherent in this scheme is that the JS code in steps 3, 5, and 7
 * might change the heap in a way that causes the GC to collect an object that
 * is actually reachable. The write barrier prevents this from happening. We use
 * a variant of incremental GC called "snapshot at the beginning." This approach
 * guarantees the invariant that if an object is reachable in step 2, then we
 * will mark it eventually. The name comes from the idea that we take a
 * theoretical "snapshot" of all reachable objects in step 2; all objects in
 * that snapshot should eventually be marked. (Note that the write barrier
 * verifier code takes an actual snapshot.)
 *
 * The basic correctness invariant of a snapshot-at-the-beginning collector is
 * that any object reachable at the end of the GC (step 9) must either:
 *   (1) have been reachable at the beginning (step 2) and thus in the snapshot
 *   (2) or must have been newly allocated, in steps 3, 5, or 7.
 * To deal with case (2), any objects allocated during an incremental GC are
 * automatically marked black.
 *
 * This strategy is actually somewhat conservative: if an object becomes
 * unreachable between steps 2 and 8, it would be safe to collect it. We won't,
 * mainly for simplicity. (Also, note that the snapshot is entirely
 * theoretical. We don't actually do anything special in step 2 that we wouldn't
 * do in a non-incremental GC.
 *
 * It's the pre-barrier's job to maintain the snapshot invariant. Consider the
 * write "obj->field = value". Let the prior value of obj->field be
 * value0. Since it's possible that value0 may have been what obj->field
 * contained in step 2, when the snapshot was taken, the barrier marks
 * value0. Note that it only does this if we're in the middle of an incremental
 * GC. Since this is rare, the cost of the write barrier is usually just an
 * extra branch.
 *
 * In practice, we implement the pre-barrier differently based on the type of
 * value0. E.g., see JSObject::writeBarrierPre, which is used if obj->field is
 * a JSObject*. It takes value0 as a parameter.
 *
 *                                POST-BARRIER
 *
 * These are not yet implemented. Once we get generational GC, they will allow
 * us to keep track of pointers from non-nursery space into the nursery.
 *
 *                            IMPLEMENTATION DETAILS
 *
 * Since it would be awkward to change every write to memory into a function
 * call, this file contains a bunch of C++ classes and templates that use
 * operator overloading to take care of barriers automatically. In many cases,
 * all that's necessary to make some field be barriered is to replace
 *     Type *field;
 * with
 *     HeapPtr<Type> field;
 * There are also special classes HeapValue and HeapId, which barrier js::Value
 * and jsid, respectively.
 *
 * One additional note: not all object writes need to be barriered. Writes to
 * newly allocated objects do not need a pre-barrier.  In these cases, we use
 * the "obj->field.init(value)" method instead of "obj->field = value". We use
 * the init naming idiom in many places to signify that a field is being
 * assigned for the first time.
 */

namespace js {

class PropertyName;

namespace gc {
// Direct value access used by the write barriers and the jits.
void
MarkValueUnbarriered(JSTracer *trc, Value *v, const char *name);

// These two declarations are also present in gc/Marking.h, via the DeclMarker
// macro.  Not great, but hard to avoid.
void
MarkObjectUnbarriered(JSTracer *trc, JSObject **obj, const char *name);
void
MarkStringUnbarriered(JSTracer *trc, JSString **str, const char *name);
}

// Note: these functions must be equivalent to the zone() functions implemented
// by all the subclasses of Cell.

JS::Zone *
ZoneOfObject(const JSObject &obj);

static inline JS::shadow::Zone *
ShadowZoneOfObject(JSObject *obj)
{
    return JS::shadow::Zone::asShadowZone(ZoneOfObject(*obj));
}

static inline JS::shadow::Zone *
ShadowZoneOfString(JSString *str)
{
    return JS::shadow::Zone::asShadowZone(reinterpret_cast<const js::gc::Cell *>(str)->tenuredZone());
}

JS_ALWAYS_INLINE JS::Zone *
ZoneOfValue(const JS::Value &value)
{
    JS_ASSERT(value.isMarkable());
    if (value.isObject())
        return ZoneOfObject(value.toObject());
    return static_cast<js::gc::Cell *>(value.toGCThing())->tenuredZone();
}

/*
 * This is a post barrier for HashTables whose key is a GC pointer. Any
 * insertion into a HashTable not marked as part of the runtime, with a GC
 * pointer as a key, must call this immediately after each insertion.
 */
template <class Map, class Key>
inline void
HashTableWriteBarrierPost(JSRuntime *rt, Map *map, const Key &key)
{
#ifdef JSGC_GENERATIONAL
    if (key && IsInsideNursery(rt, key)) {
        JS::shadow::Runtime *shadowRuntime = JS::shadow::Runtime::asShadowRuntime(rt);
        shadowRuntime->gcStoreBufferPtr()->putGeneric(gc::HashKeyRef<Map, Key>(map, key));
    }
#endif
}

template<class T, typename Unioned = uintptr_t>
class EncapsulatedPtr
{
  protected:
    union {
        T *value;
        Unioned other;
    };

  public:
    EncapsulatedPtr() : value(NULL) {}
    EncapsulatedPtr(T *v) : value(v) {}
    explicit EncapsulatedPtr(const EncapsulatedPtr<T> &v) : value(v.value) {}

    ~EncapsulatedPtr() { pre(); }

    void init(T *v) {
        JS_ASSERT(!IsPoisonedPtr<T>(v));
        this->value = v;
    }

    /* Use to set the pointer to NULL. */
    void clear() {
        pre();
        value = NULL;
    }

    EncapsulatedPtr<T, Unioned> &operator=(T *v) {
        pre();
        JS_ASSERT(!IsPoisonedPtr<T>(v));
        value = v;
        return *this;
    }

    EncapsulatedPtr<T, Unioned> &operator=(const EncapsulatedPtr<T> &v) {
        pre();
        JS_ASSERT(!IsPoisonedPtr<T>(v.value));
        value = v.value;
        return *this;
    }

    /* Use this if the automatic coercion to T* isn't working. */
    T *get() const { return value; }

    /*
     * Use these if you want to change the value without invoking the barrier.
     * Obviously this is dangerous unless you know the barrier is not needed.
     */
    T **unsafeGet() { return &value; }
    void unsafeSet(T *v) { value = v; }

    Unioned *unsafeGetUnioned() { return &other; }

    T &operator*() const { return *value; }
    T *operator->() const { return value; }

    operator T*() const { return value; }

  protected:
    void pre() { T::writeBarrierPre(value); }
};

template <class T, class Unioned = uintptr_t>
class HeapPtr : public EncapsulatedPtr<T, Unioned>
{
  public:
    HeapPtr() : EncapsulatedPtr<T>(NULL) {}
    explicit HeapPtr(T *v) : EncapsulatedPtr<T>(v) { post(); }
    explicit HeapPtr(const HeapPtr<T> &v)
      : EncapsulatedPtr<T>(v) { post(); }

    void init(T *v) {
        JS_ASSERT(!IsPoisonedPtr<T>(v));
        this->value = v;
        post();
    }

    HeapPtr<T, Unioned> &operator=(T *v) {
        this->pre();
        JS_ASSERT(!IsPoisonedPtr<T>(v));
        this->value = v;
        post();
        return *this;
    }

    HeapPtr<T, Unioned> &operator=(const HeapPtr<T> &v) {
        this->pre();
        JS_ASSERT(!IsPoisonedPtr<T>(v.value));
        this->value = v.value;
        post();
        return *this;
    }

  protected:
    void post() { T::writeBarrierPost(this->value, (void *)&this->value); }

    /* Make this friend so it can access pre() and post(). */
    template<class T1, class T2>
    friend inline void
    BarrieredSetPair(Zone *zone,
                     HeapPtr<T1> &v1, T1 *val1,
                     HeapPtr<T2> &v2, T2 *val2);
};

/*
 * FixedHeapPtr is designed for one very narrow case: replacing immutable raw
 * pointers to GC-managed things, implicitly converting to a handle type for
 * ease of use.  Pointers encapsulated by this type must:
 *
 *   be immutable (no incremental write barriers),
 *   never point into the nursery (no generational write barriers), and
 *   be traced via MarkRuntime (we use fromMarkedLocation).
 *
 * In short: you *really* need to know what you're doing before you use this
 * class!
 */
template <class T>
class FixedHeapPtr
{
    T *value;

  public:
    operator T*() const { return value; }
    T * operator->() const { return value; }

    operator Handle<T*>() const {
        return Handle<T*>::fromMarkedLocation(&value);
    }

    void init(T *ptr) {
        value = ptr;
    }
};

template <class T>
class RelocatablePtr : public EncapsulatedPtr<T>
{
  public:
    RelocatablePtr() : EncapsulatedPtr<T>(NULL) {}
    explicit RelocatablePtr(T *v) : EncapsulatedPtr<T>(v) {
        if (v)
            post();
    }
    RelocatablePtr(const RelocatablePtr<T> &v) : EncapsulatedPtr<T>(v) {
        if (this->value)
            post();
    }

    ~RelocatablePtr() {
        if (this->value)
            relocate(this->value->runtimeFromMainThread());
    }

    RelocatablePtr<T> &operator=(T *v) {
        this->pre();
        JS_ASSERT(!IsPoisonedPtr<T>(v));
        if (v) {
            this->value = v;
            post();
        } else if (this->value) {
            JSRuntime *rt = this->value->runtimeFromMainThread();
            this->value = v;
            relocate(rt);
        }
        return *this;
    }

    RelocatablePtr<T> &operator=(const RelocatablePtr<T> &v) {
        this->pre();
        JS_ASSERT(!IsPoisonedPtr<T>(v.value));
        if (v.value) {
            this->value = v.value;
            post();
        } else if (this->value) {
            JSRuntime *rt = this->value->runtimeFromMainThread();
            this->value = v;
            relocate(rt);
        }
        return *this;
    }

  protected:
    void post() {
#ifdef JSGC_GENERATIONAL
        JS_ASSERT(this->value);
        T::writeBarrierPostRelocate(this->value, &this->value);
#endif
    }

    void relocate(JSRuntime *rt) {
#ifdef JSGC_GENERATIONAL
        T::writeBarrierPostRemove(this->value, &this->value);
#endif
    }
};

/*
 * This is a hack for RegExpStatics::updateFromMatch. It allows us to do two
 * barriers with only one branch to check if we're in an incremental GC.
 */
template<class T1, class T2>
static inline void
BarrieredSetPair(Zone *zone,
                 HeapPtr<T1> &v1, T1 *val1,
                 HeapPtr<T2> &v2, T2 *val2)
{
    if (T1::needWriteBarrierPre(zone)) {
        v1.pre();
        v2.pre();
    }
    v1.unsafeSet(val1);
    v2.unsafeSet(val2);
    v1.post();
    v2.post();
}

class Shape;
class BaseShape;
namespace types { struct TypeObject; }

typedef EncapsulatedPtr<JSObject> EncapsulatedPtrObject;
typedef EncapsulatedPtr<JSScript> EncapsulatedPtrScript;

typedef RelocatablePtr<JSObject> RelocatablePtrObject;
typedef RelocatablePtr<JSScript> RelocatablePtrScript;

typedef HeapPtr<JSObject> HeapPtrObject;
typedef HeapPtr<JSFunction> HeapPtrFunction;
typedef HeapPtr<JSString> HeapPtrString;
typedef HeapPtr<PropertyName> HeapPtrPropertyName;
typedef HeapPtr<JSScript> HeapPtrScript;
typedef HeapPtr<Shape> HeapPtrShape;
typedef HeapPtr<BaseShape> HeapPtrBaseShape;
typedef HeapPtr<types::TypeObject> HeapPtrTypeObject;

/* Useful for hashtables with a HeapPtr as key. */
template<class T>
struct HeapPtrHasher
{
    typedef HeapPtr<T> Key;
    typedef T *Lookup;

    static HashNumber hash(Lookup obj) { return DefaultHasher<T *>::hash(obj); }
    static bool match(const Key &k, Lookup l) { return k.get() == l; }
    static void rekey(Key &k, const Key& newKey) { k.unsafeSet(newKey); }
};

/* Specialized hashing policy for HeapPtrs. */
template <class T>
struct DefaultHasher< HeapPtr<T> > : HeapPtrHasher<T> { };

template<class T>
struct EncapsulatedPtrHasher
{
    typedef EncapsulatedPtr<T> Key;
    typedef T *Lookup;

    static HashNumber hash(Lookup obj) { return DefaultHasher<T *>::hash(obj); }
    static bool match(const Key &k, Lookup l) { return k.get() == l; }
    static void rekey(Key &k, const Key& newKey) { k.unsafeSet(newKey); }
};

template <class T>
struct DefaultHasher< EncapsulatedPtr<T> > : EncapsulatedPtrHasher<T> { };

class EncapsulatedValue : public ValueOperations<EncapsulatedValue>
{
  protected:
    Value value;

    /*
     * Ensure that EncapsulatedValue is not constructable, except by our
     * implementations.
     */
    EncapsulatedValue() MOZ_DELETE;

  public:
    EncapsulatedValue(const Value &v) : value(v) {
        JS_ASSERT(!IsPoisonedValue(v));
    }
    EncapsulatedValue(const EncapsulatedValue &v) : value(v) {
        JS_ASSERT(!IsPoisonedValue(v));
    }

    ~EncapsulatedValue() {
        pre();
    }

    void init(const Value &v) {
        JS_ASSERT(!IsPoisonedValue(v));
        value = v;
    }
    void init(JSRuntime *rt, const Value &v) {
        JS_ASSERT(!IsPoisonedValue(v));
        value = v;
    }

    EncapsulatedValue &operator=(const Value &v) {
        pre();
        JS_ASSERT(!IsPoisonedValue(v));
        value = v;
        return *this;
    }

    EncapsulatedValue &operator=(const EncapsulatedValue &v) {
        pre();
        JS_ASSERT(!IsPoisonedValue(v));
        value = v.get();
        return *this;
    }

    bool operator==(const EncapsulatedValue &v) const { return value == v.value; }
    bool operator!=(const EncapsulatedValue &v) const { return value != v.value; }

    const Value &get() const { return value; }
    Value *unsafeGet() { return &value; }
    operator const Value &() const { return value; }

    JSGCTraceKind gcKind() const { return value.gcKind(); }

    uint64_t asRawBits() const { return value.asRawBits(); }

    static void writeBarrierPre(const Value &v) {
#ifdef JSGC_INCREMENTAL
        if (v.isMarkable() && shadowRuntimeFromAnyThread(v)->needsBarrier())
            writeBarrierPre(ZoneOfValue(v), v);
#endif
    }

    static void writeBarrierPre(Zone *zone, const Value &v) {
#ifdef JSGC_INCREMENTAL
        JS::shadow::Zone *shadowZone = JS::shadow::Zone::asShadowZone(zone);
        if (shadowZone->needsBarrier()) {
            JS_ASSERT_IF(v.isMarkable(), shadowRuntimeFromMainThread(v)->needsBarrier());
            Value tmp(v);
            js::gc::MarkValueUnbarriered(shadowZone->barrierTracer(), &tmp, "write barrier");
            JS_ASSERT(tmp == v);
        }
#endif
    }

  protected:
    void pre() { writeBarrierPre(value); }
    void pre(Zone *zone) { writeBarrierPre(zone, value); }

    static JSRuntime *runtimeFromMainThread(const Value &v) {
        JS_ASSERT(v.isMarkable());
        return static_cast<js::gc::Cell *>(v.toGCThing())->runtimeFromMainThread();
    }
    static JSRuntime *runtimeFromAnyThread(const Value &v) {
        JS_ASSERT(v.isMarkable());
        return static_cast<js::gc::Cell *>(v.toGCThing())->runtimeFromAnyThread();
    }
    static JS::shadow::Runtime *shadowRuntimeFromMainThread(const Value &v) {
        return reinterpret_cast<JS::shadow::Runtime*>(runtimeFromMainThread(v));
    }
    static JS::shadow::Runtime *shadowRuntimeFromAnyThread(const Value &v) {
        return reinterpret_cast<JS::shadow::Runtime*>(runtimeFromAnyThread(v));
    }

  private:
    friend class ValueOperations<EncapsulatedValue>;
    const Value * extract() const { return &value; }
};

class HeapValue : public EncapsulatedValue
{
  public:
    explicit HeapValue()
      : EncapsulatedValue(UndefinedValue())
    {
        post();
    }

    explicit HeapValue(const Value &v)
      : EncapsulatedValue(v)
    {
        JS_ASSERT(!IsPoisonedValue(v));
        post();
    }

    explicit HeapValue(const HeapValue &v)
      : EncapsulatedValue(v.value)
    {
        JS_ASSERT(!IsPoisonedValue(v.value));
        post();
    }

    ~HeapValue() {
        pre();
    }

    void init(const Value &v) {
        JS_ASSERT(!IsPoisonedValue(v));
        value = v;
        post();
    }

    void init(JSRuntime *rt, const Value &v) {
        JS_ASSERT(!IsPoisonedValue(v));
        value = v;
        post(rt);
    }

    HeapValue &operator=(const Value &v) {
        pre();
        JS_ASSERT(!IsPoisonedValue(v));
        value = v;
        post();
        return *this;
    }

    HeapValue &operator=(const HeapValue &v) {
        pre();
        JS_ASSERT(!IsPoisonedValue(v.value));
        value = v.value;
        post();
        return *this;
    }

#ifdef DEBUG
    bool preconditionForSet(Zone *zone);
#endif

    /*
     * This is a faster version of operator=. Normally, operator= has to
     * determine the compartment of the value before it can decide whether to do
     * the barrier. If you already know the compartment, it's faster to pass it
     * in.
     */
    void set(Zone *zone, const Value &v) {
        JS::shadow::Zone *shadowZone = JS::shadow::Zone::asShadowZone(zone);
        JS_ASSERT(preconditionForSet(zone));
        pre(zone);
        JS_ASSERT(!IsPoisonedValue(v));
        value = v;
        post(shadowZone->runtimeFromAnyThread());
    }

    static void writeBarrierPost(const Value &value, Value *addr) {
#ifdef JSGC_GENERATIONAL
        if (value.isMarkable())
            shadowRuntimeFromMainThread(value)->gcStoreBufferPtr()->putValue(addr);
#endif
    }

    static void writeBarrierPost(JSRuntime *rt, const Value &value, Value *addr) {
#ifdef JSGC_GENERATIONAL
        if (value.isMarkable()) {
            JS::shadow::Runtime *shadowRuntime = JS::shadow::Runtime::asShadowRuntime(rt);
            shadowRuntime->gcStoreBufferPtr()->putValue(addr);
        }
#endif
    }

  private:
    void post() {
        writeBarrierPost(value, &value);
    }

    void post(JSRuntime *rt) {
        writeBarrierPost(rt, value, &value);
    }
};

class RelocatableValue : public EncapsulatedValue
{
  public:
    explicit RelocatableValue()
      : EncapsulatedValue(UndefinedValue())
    {}

    explicit RelocatableValue(const Value &v)
      : EncapsulatedValue(v)
    {
        JS_ASSERT(!IsPoisonedValue(v));
        if (v.isMarkable())
            post();
    }

    RelocatableValue(const RelocatableValue &v)
      : EncapsulatedValue(v.value)
    {
        JS_ASSERT(!IsPoisonedValue(v.value));
        if (v.value.isMarkable())
            post();
    }

    ~RelocatableValue()
    {
        if (value.isMarkable())
            relocate(runtimeFromMainThread(value));
    }

    RelocatableValue &operator=(const Value &v) {
        pre();
        JS_ASSERT(!IsPoisonedValue(v));
        if (v.isMarkable()) {
            value = v;
            post();
        } else if (value.isMarkable()) {
            JSRuntime *rt = runtimeFromMainThread(value);
            relocate(rt);
            value = v;
        } else {
            value = v;
        }
        return *this;
    }

    RelocatableValue &operator=(const RelocatableValue &v) {
        pre();
        JS_ASSERT(!IsPoisonedValue(v.value));
        if (v.value.isMarkable()) {
            value = v.value;
            post();
        } else if (value.isMarkable()) {
            JSRuntime *rt = runtimeFromMainThread(value);
            relocate(rt);
            value = v.value;
        } else {
            value = v.value;
        }
        return *this;
    }

  private:
    void post() {
#ifdef JSGC_GENERATIONAL
        JS_ASSERT(value.isMarkable());
        shadowRuntimeFromMainThread(value)->gcStoreBufferPtr()->putRelocatableValue(&value);
#endif
    }

    void relocate(JSRuntime *rt) {
#ifdef JSGC_GENERATIONAL
        JS::shadow::Runtime *shadowRuntime = JS::shadow::Runtime::asShadowRuntime(rt);
        shadowRuntime->gcStoreBufferPtr()->removeRelocatableValue(&value);
#endif
    }
};

class HeapSlot : public EncapsulatedValue
{
    /*
     * Operator= is not valid for HeapSlot because is must take the object and
     * slot offset to provide to the post/generational barrier.
     */
    inline HeapSlot &operator=(const Value &v) MOZ_DELETE;
    inline HeapSlot &operator=(const HeapValue &v) MOZ_DELETE;
    inline HeapSlot &operator=(const HeapSlot &v) MOZ_DELETE;

  public:
    enum Kind {
        Slot,
        Element
    };

    explicit HeapSlot() MOZ_DELETE;

    explicit HeapSlot(JSObject *obj, Kind kind, uint32_t slot, const Value &v)
      : EncapsulatedValue(v)
    {
        JS_ASSERT(!IsPoisonedValue(v));
        post(obj, kind, slot, v);
    }

    explicit HeapSlot(JSObject *obj, Kind kind, uint32_t slot, const HeapSlot &s)
      : EncapsulatedValue(s.value)
    {
        JS_ASSERT(!IsPoisonedValue(s.value));
        post(obj, kind, slot, s);
    }

    ~HeapSlot() {
        pre();
    }

    void init(JSObject *owner, Kind kind, uint32_t slot, const Value &v) {
        value = v;
        post(owner, kind, slot, v);
    }

    void init(JSRuntime *rt, JSObject *owner, Kind kind, uint32_t slot, const Value &v) {
        value = v;
        post(rt, owner, kind, slot, v);
    }

#ifdef DEBUG
    bool preconditionForSet(JSObject *owner, Kind kind, uint32_t slot);
    bool preconditionForSet(Zone *zone, JSObject *owner, Kind kind, uint32_t slot);
    static void preconditionForWriteBarrierPost(JSObject *obj, Kind kind, uint32_t slot,
                                                Value target);
#endif

    void set(JSObject *owner, Kind kind, uint32_t slot, const Value &v) {
        JS_ASSERT(preconditionForSet(owner, kind, slot));
        pre();
        JS_ASSERT(!IsPoisonedValue(v));
        value = v;
        post(owner, kind, slot, v);
    }

    void set(Zone *zone, JSObject *owner, Kind kind, uint32_t slot, const Value &v) {
        JS_ASSERT(preconditionForSet(zone, owner, kind, slot));
        JS::shadow::Zone *shadowZone = JS::shadow::Zone::asShadowZone(zone);
        pre(zone);
        JS_ASSERT(!IsPoisonedValue(v));
        value = v;
        post(shadowZone->runtimeFromAnyThread(), owner, kind, slot, v);
    }

    static void writeBarrierPost(JSObject *obj, Kind kind, uint32_t slot, Value target)
    {
#ifdef JSGC_GENERATIONAL
        js::gc::Cell *cell = reinterpret_cast<js::gc::Cell*>(obj);
        writeBarrierPost(cell->runtimeFromAnyThread(), obj, kind, slot, target);
#endif
    }

    static void writeBarrierPost(JSRuntime *rt, JSObject *obj, Kind kind, uint32_t slot,
                                 Value target)
    {
#ifdef DEBUG
        preconditionForWriteBarrierPost(obj, kind, slot, target);
#endif
#ifdef JSGC_GENERATIONAL
        if (target.isObject()) {
            JS::shadow::Runtime *shadowRuntime = JS::shadow::Runtime::asShadowRuntime(rt);
            shadowRuntime->gcStoreBufferPtr()->putSlot(obj, kind, slot, &target.toObject());
        }
#endif
    }

  private:
    void post(JSObject *owner, Kind kind, uint32_t slot, Value target) {
        HeapSlot::writeBarrierPost(owner, kind, slot, target);
    }

    void post(JSRuntime *rt, JSObject *owner, Kind kind, uint32_t slot, Value target) {
        HeapSlot::writeBarrierPost(rt, owner, kind, slot, target);
    }
};

static inline const Value *
Valueify(const EncapsulatedValue *array)
{
    JS_STATIC_ASSERT(sizeof(HeapValue) == sizeof(Value));
    JS_STATIC_ASSERT(sizeof(HeapSlot) == sizeof(Value));
    return (const Value *)array;
}

static inline HeapValue *
HeapValueify(Value *v)
{
    JS_STATIC_ASSERT(sizeof(HeapValue) == sizeof(Value));
    JS_STATIC_ASSERT(sizeof(HeapSlot) == sizeof(Value));
    return (HeapValue *)v;
}

class HeapSlotArray
{
    HeapSlot *array;

  public:
    HeapSlotArray(HeapSlot *array) : array(array) {}

    operator const Value *() const { return Valueify(array); }
    operator HeapSlot *() const { return array; }

    HeapSlotArray operator +(int offset) const { return HeapSlotArray(array + offset); }
    HeapSlotArray operator +(uint32_t offset) const { return HeapSlotArray(array + offset); }
};

class EncapsulatedId
{
  protected:
    jsid value;

  private:
    EncapsulatedId(const EncapsulatedId &v) MOZ_DELETE;

  public:
    explicit EncapsulatedId() : value(JSID_VOID) {}
    explicit EncapsulatedId(jsid id) : value(id) {}
    ~EncapsulatedId() { pre(); }

    EncapsulatedId &operator=(const EncapsulatedId &v) {
        if (v.value != value)
            pre();
        JS_ASSERT(!IsPoisonedId(v.value));
        value = v.value;
        return *this;
    }

    bool operator==(jsid id) const { return value == id; }
    bool operator!=(jsid id) const { return value != id; }

    jsid get() const { return value; }
    jsid *unsafeGet() { return &value; }
    void unsafeSet(jsid newId) { value = newId; }
    operator jsid() const { return value; }

  protected:
    void pre() {
#ifdef JSGC_INCREMENTAL
        if (JSID_IS_OBJECT(value)) {
            JSObject *obj = JSID_TO_OBJECT(value);
            JS::shadow::Zone *shadowZone = ShadowZoneOfObject(obj);
            if (shadowZone->needsBarrier()) {
                js::gc::MarkObjectUnbarriered(shadowZone->barrierTracer(), &obj, "write barrier");
                JS_ASSERT(obj == JSID_TO_OBJECT(value));
            }
        } else if (JSID_IS_STRING(value)) {
            JSString *str = JSID_TO_STRING(value);
            JS::shadow::Zone *shadowZone = ShadowZoneOfString(str);
            if (shadowZone->needsBarrier()) {
                js::gc::MarkStringUnbarriered(shadowZone->barrierTracer(), &str, "write barrier");
                JS_ASSERT(str == JSID_TO_STRING(value));
            }
        }
#endif
    }
};

class RelocatableId : public EncapsulatedId
{
  public:
    explicit RelocatableId() : EncapsulatedId() {}
    explicit inline RelocatableId(jsid id) : EncapsulatedId(id) {}
    ~RelocatableId() { pre(); }

    RelocatableId &operator=(jsid id) {
        if (id != value)
            pre();
        JS_ASSERT(!IsPoisonedId(id));
        value = id;
        return *this;
    }

    RelocatableId &operator=(const RelocatableId &v) {
        if (v.value != value)
            pre();
        JS_ASSERT(!IsPoisonedId(v.value));
        value = v.value;
        return *this;
    }
};

class HeapId : public EncapsulatedId
{
  public:
    explicit HeapId() : EncapsulatedId() {}

    explicit HeapId(jsid id)
      : EncapsulatedId(id)
    {
        JS_ASSERT(!IsPoisonedId(id));
        post();
    }

    ~HeapId() { pre(); }

    void init(jsid id) {
        JS_ASSERT(!IsPoisonedId(id));
        value = id;
        post();
    }

    HeapId &operator=(jsid id) {
        if (id != value)
            pre();
        JS_ASSERT(!IsPoisonedId(id));
        value = id;
        post();
        return *this;
    }

    HeapId &operator=(const HeapId &v) {
        if (v.value != value)
            pre();
        JS_ASSERT(!IsPoisonedId(v.value));
        value = v.value;
        post();
        return *this;
    }

  private:
    void post() {};

    HeapId(const HeapId &v) MOZ_DELETE;
};

/*
 * Incremental GC requires that weak pointers have read barriers. This is mostly
 * an issue for empty shapes stored in JSCompartment. The problem happens when,
 * during an incremental GC, some JS code stores one of the compartment's empty
 * shapes into an object already marked black. Normally, this would not be a
 * problem, because the empty shape would have been part of the initial snapshot
 * when the GC started. However, since this is a weak pointer, it isn't. So we
 * may collect the empty shape even though a live object points to it. To fix
 * this, we mark these empty shapes black whenever they get read out.
 */
template<class T>
class ReadBarriered
{
    T *value;

  public:
    ReadBarriered() : value(NULL) {}
    ReadBarriered(T *value) : value(value) {}
    ReadBarriered(const Rooted<T*> &rooted) : value(rooted) {}

    T *get() const {
        if (!value)
            return NULL;
        T::readBarrier(value);
        return value;
    }

    operator T*() const { return get(); }

    T &operator*() const { return *get(); }
    T *operator->() const { return get(); }

    T **unsafeGet() { return &value; }
    T * const * unsafeGet() const { return &value; }

    void set(T *v) { value = v; }

    operator bool() { return !!value; }
};

class ReadBarrieredValue
{
    Value value;

  public:
    ReadBarrieredValue() : value(UndefinedValue()) {}
    ReadBarrieredValue(const Value &value) : value(value) {}

    inline const Value &get() const;
    Value *unsafeGet() { return &value; }
    inline operator const Value &() const;

    inline JSObject &toObject() const;
};

#ifdef DEBUG
bool
RuntimeFromMainThreadIsHeapMajorCollecting(JS::shadow::Zone *shadowZone);
#endif

} /* namespace js */

#endif /* gc_Barrier_h */
